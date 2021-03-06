/****************************************************************************
    Markus Eich
    DFKI 2013
    markus.eich@dfki.de
****************************************************************************/

#include "PointCloudDetection.hpp"
#include <string>

using namespace object_detection;

PointCloudDetection::PointCloudDetection():debug_(false),use_color_(false),ground_level_(0.0)
{
    //todo move to config file

    //cluster size for every detectable cluster (plane, cylinder, ground)
    min_cluster_size_=50;
    max_cluster_size_=50000;
    max_cluster_dist_=0.03;

    // RanSac params
    ransac_iterations_=10000;
    ransac_dist_threshold_=0.03;
    ransac_normal_dist_weight_=0.1;
    ransac_min_radius_=0.01;
    ransac_max_radius_=0.10;

    cylinderModelSet_=false;
    boxModelSet_=false;
    use_cylinder_orientation_=false;

    srand (time(NULL));
}

/**
  * @brief sets the internal point cloud
  * @param points the pointcloud
  */
void PointCloudDetection::setPointcloud(pcl::PointCloud<PointT>::Ptr points){
    pointcloud_.clear();
    pcl::PointCloud<PointT>::iterator iter;
    PointRGBT color_pt(255,255,255); //default color white if no color is given
    for (iter=points->begin();iter!=points->end();iter++){
        color_pt.x=iter->x;
        color_pt.y=iter->y;
        color_pt.z=iter->z;
        pointcloud_.push_back(color_pt);
    }

    this->computeNormals();

}
/**
  * @brief sets the internal point cloud
  * @param points the pointcloud
  */

void PointCloudDetection::setPointcloud(pcl::PointCloud<PointRGBT>::Ptr points){
    pointcloud_.clear();
    pcl::PointCloud<PointRGBT>::iterator iter;
    for (iter=points->begin();iter!=points->end();iter++){
        PointRGBT color_pt(iter->r,iter->g,iter->b);
        color_pt.x=iter->x;
        color_pt.y=iter->y;
        color_pt.z=iter->z;
        pointcloud_.push_back(color_pt);
    }
    this->computeNormals();
}
/**
  * @brief compute normals for the given point cloud
  * @return true if successful
  */
bool PointCloudDetection::computeNormals()
{

    if (pointcloud_.size()==0){
        PCL_ERROR("No points to compute normals\n");
        return false;
    }
    else{
    //calculate point normals
    //for the Ransac using plane normals
    cloud_normals_.clear();
    pcl::NormalEstimation<PointRGBT, pcl::Normal> ne;
    // Estimate point normals
    ne.setSearchMethod (boost::make_shared<pcl::search::KdTree<PointRGBT> > ());
    ne.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> >(pointcloud_));
    ne.setKSearch (30);
    ne.compute (cloud_normals_);
    return true;
    }
}

/**
  * @brief Removes larger plane from the pointcloud. Used for removing ground planes
  * @param minSize minimum size of the plane which is removed in sqm. should be larger than the size of the box
  */
void PointCloudDetection::removeHorizontalPlanes(double minExtension, double maxHorizVariance)
{        
    if (pointcloud_.size()==0){
        std::cout<<"Error: No points available"<<std::endl;
        return;
    }
    //use region growing, more robust than RanSaC
    pcl::RegionGrowing<PointRGBT, pcl::Normal> reg;
    //search Tree (KD Tree)
    pcl::search::Search<PointRGBT>::Ptr tree = boost::shared_ptr<pcl::search::Search<PointRGBT> > (new pcl::search::KdTree<PointRGBT>);

    reg.setMinClusterSize (min_cluster_size_);
    reg.setMaxClusterSize (max_cluster_size_);
    reg.setSearchMethod (tree);
    reg.setNumberOfNeighbours (30);

    Eigen::Vector3d up(0,0,1);
    Eigen::Vector3d down(0,0,-1);
    bool planeRemoved;    

    do{

    planeRemoved=false;
    reg.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> >(pointcloud_));

    reg.setInputNormals (boost::make_shared<pcl::PointCloud<pcl::Normal> >(cloud_normals_));
    reg.setSmoothnessThreshold (config.smoothness/ 180.0 * M_PI);
    reg.setCurvatureThreshold (config.curvature);

    std::vector <pcl::PointIndices> clusters;
    reg.extract (clusters);
    std::vector<pcl::PointIndices>::iterator iter;


    for (iter=clusters.begin();iter!=clusters.end();iter++){

        pcl::PointCloud<PointRGBT> cloud_plane(pointcloud_,iter->indices);        
        segmentation::Plane plane;

        for (pcl::PointCloud<PointRGBT>::iterator pIter=cloud_plane.begin();pIter!=cloud_plane.end();pIter++){
                  plane.addPoint(segmentation::Shared_Point(new segmentation::Point((double)pIter->x,(double)pIter->y,(double)pIter->z)));
        }


        Eigen::Vector3d planeNormal=plane.getNormalVector();

        double maxPlaneExtension=std::max(plane.getMaxExpansion().width,plane.getMaxExpansion().height);


        double VarianceUp = atan2(planeNormal.cross(up).norm(),planeNormal.dot(up));
        double VarianceDown = atan2(planeNormal.cross(down).norm(),planeNormal.dot(down));

        double horVariance=std::min(VarianceUp,VarianceDown);


        if (debug_) PCL_WARN("Plane Extension : %f\n",maxPlaneExtension);
        if (debug_) PCL_WARN("Horizontal Variance: %f\n",horVariance);

        if ((maxPlaneExtension>minExtension) && (horVariance<maxHorizVariance)){
            if (plane.getPosition().z()<ground_level_)
                ground_level_=plane.getPosition().z();

            pcl::ExtractIndices<PointRGBT> ext;
            ext.setInputCloud(boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
            ext.setIndices(boost::make_shared<std::vector<int> > (iter->indices));
            ext.setNegative(true);
            ext.filter(pointcloud_);
            this->computeNormals();
            planeRemoved=true;
            if (debug_) PCL_WARN("Ground plane removed\n");
            break;
        }
    }

    }while(planeRemoved);


    return;
}
/**
  * @brief Get a list of all extracted cylinders
  * @param output list of all cyinders
  * @return found cylinders
  */
bool PointCloudDetection::getCylinders(std::vector<object_detection::Cylinder>& cylinders)
{
    //TODO move Cylinder stuff to a separate class later on

    //take average color of component
    double r=0;
    double g=0;
    double b=0;

    Eigen::Vector3i point_color;
    object_detection::Color avg_color;

    if (!cylinderModelSet_) return false;

    boost::shared_ptr<std::vector<pcl::PointCloud<PointRGBT> > > clusters(new std::vector<pcl::PointCloud<PointRGBT> >());
    std::vector<pcl::PointCloud<PointRGBT> >::iterator iter;

    //clear everything
    cylinder_clusters_.clear();
    cylinders.clear();

    this->getClusters(clusters);

    if (clusters->size()==0){
        PCL_WARN("No clusters found\n");
        return false;
    }

    if (debug_) PCL_WARN("Found %d clusters\n",clusters->size());

    //go through all the clusters and check if the cluster represents a cylinder matching the given model

    iter=clusters->begin();

    double likelihood; //overall likelihood for matching the shape model

    PointT maxPt;
    PointT minPt;
    pcl::ModelCoefficients coefficients;

    for (;iter!=clusters->end();iter++){

        Cylinder cylinder;

        //get the average color of the cluster
        BOOST_FOREACH(pcl::PointXYZRGB point,*iter){
            point_color=point.getRGBVector3i();
            r+=point_color[0]/255;
            g+=point_color[1]/255;
            b+=point_color[2]/255;
        }

        r/=iter->size();
        g/=iter->size();
        b/=iter->size();
        avg_color=object_detection::Color(r,g,b);


        //fill cylinder object
        cylinder.shapeMatch=getCylinderLikelihood(boost::make_shared<pcl::PointCloud<PointRGBT> >(*iter),coefficients);
        cylinder.height=getCylinderHeight(boost::make_shared<pcl::PointCloud<PointRGBT> >(*iter),maxPt,minPt,coefficients);
        cylinder.diameter=coefficients.values[6]*2;



        //do some crazy conversion between euler and quaternion
        //todo check if valid ok
        base::Vector3d axis_vector (coefficients.values[3],coefficients.values[4],coefficients.values[5]);
        cylinder.axis=axis_vector;                
        cylinder.position=Eigen::Vector3d((maxPt.x+minPt.x)/2,(maxPt.y+minPt.y)/2,(maxPt.z+minPt.z)/2);

        base::Vector3d up_vector(0.0, 0.0, 1.0);
        base::Vector3d right_vector = axis_vector.cross(up_vector);
        right_vector.normalize();

        double angle=-1.0*acos(axis_vector.dot(up_vector));
        double s = sin(angle * 0.5);

        cylinder.orientation=base::Quaterniond(right_vector[0] * s, right_vector[1] * s, right_vector[2] * s, cos(angle * 0.5));
        cylinder.orientation.normalize();

        float cos_theta = (-cylinder.position[0] * cylinder.axis[0] + -cylinder.position[1] * cylinder.axis[1] + -cylinder.position[2] * cylinder.axis[2]);

        // Flip the normals if needed
        if (cos_theta < 0) cylinder.axis*=-1;

        likelihood=this->getCylinderScore(cylinder);


        if (debug_){
            PCL_WARN("Cylinder Candidate: diameter: %f, height: %f, likelihood: %f, vertical: %f \n",cylinder.diameter,cylinder.height,likelihood,this->isVertical(cylinder.axis));
        }


        if (!use_cylinder_orientation_ || (use_cylinder_orientation_ && (this->isVertical(cylinder.axis)>0.8))){

            if (use_color_){
                if ((likelihood>config.cylinderModelThreshold) && (this->matchesColor(avg_color,cylinderModel_.color)>0.8)){
                    cylinder.likelihood=likelihood;
                    cylinders.push_back(cylinder);
                    cylinder_clusters_.push_back(*iter);
                }
            }
            else{
                if (likelihood>config.cylinderModelThreshold){
                    cylinder.likelihood=likelihood;
                    cylinders.push_back(cylinder);
                    cylinder_clusters_.push_back(*iter);
                }
             }
         }
    }

    if (cylinders.size()>0)
        return true;
    else
        return false;
}

/**
  * @brief Get a list of all extracted boxes
  * @param output list of all boxes
  * @return found boxes
  */
bool PointCloudDetection::getBoxes(std::vector<object_detection::Box>& boxes)
{

    if (!boxModelSet_) return false;        

    //clear everything
    boxes.clear();
    box_clusters_.clear();

    //use region growing, more robust than RanSaC
    pcl::RegionGrowing<PointRGBT, pcl::Normal> reg;
    //search Tree (KD Tree)
    pcl::search::Search<PointRGBT>::Ptr tree = boost::shared_ptr<pcl::search::Search<PointRGBT> > (new pcl::search::KdTree<PointRGBT>);

    reg.setMinClusterSize (min_cluster_size_);
    reg.setMaxClusterSize (max_cluster_size_);
    reg.setSearchMethod (tree);
    reg.setNumberOfNeighbours (30);

    reg.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> >(pointcloud_));
    reg.setInputNormals (boost::make_shared<pcl::PointCloud<pcl::Normal> >(cloud_normals_));
    reg.setSmoothnessThreshold (config.smoothness/ 180.0 * M_PI);
    reg.setCurvatureThreshold (config.curvature);

    std::vector <pcl::PointIndices> clusters;
    reg.extract (clusters);
    std::vector<pcl::PointIndices>::iterator iter;

    for (iter=clusters.begin();iter!=clusters.end();iter++){

        Box box;
        //object_detection::Plane plane(
        pcl::PointCloud<PointRGBT> cloud_plane(pointcloud_,iter->indices);
        segmentation::Plane plane;
        plane.points.clear();
        for (pcl::PointCloud<PointRGBT>::iterator pIter=cloud_plane.begin();pIter!=cloud_plane.end();pIter++){
            plane.addPoint(segmentation::Shared_Point(new segmentation::Point((double)pIter->x,(double)pIter->y,(double)pIter->z)));
        }        


        box.orientation=plane.getQuaternion();
        box.orientation.normalize();
        box.axis=plane.getNormalVector();
        box.position=plane.getCoG();
        box.dimensions[0]=std::max(plane.getMaxExpansion().width,plane.getMaxExpansion().height);
        box.dimensions[1]=std::min(plane.getMaxExpansion().width,plane.getMaxExpansion().height);

        box.position = box.position-box.axis*boxModel_.dimensions[2]/2.;

        double likelihood=getBoxScore(box,plane.getRectangularness());        

        if (debug_)
            PCL_WARN("Box Candidate width: %f, length: %f, rect: %f, likelihood: %f \n",box.dimensions[0],box.dimensions[1],plane.getRectangularness(),likelihood);

        if (likelihood>config.boxModelThreshold){
            box.likelihood=likelihood;
            //assume we have the object so set height to model. Not nice but solves the spacebot problem
            box.dimensions[2]=boxModel_.dimensions[2];
            boxes.push_back(box);
            //copy box front plane
            box_clusters_.push_back(cloud_plane);
        }
     }



    return true;

}
/**
  * @brief apply PCL passthrough filter
  * @param x size in x in meter
  * @param y size in y in meter
  * @param z size in z in meter
  */
void PointCloudDetection::applyPassthroughFilter(double x, double y, double z){

    pcl::PassThrough<PointRGBT> pass;
    pass.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
    pass.setFilterFieldName("x");
    pass.setFilterLimits(0,x);
    pass.filter (pointcloud_);
    pass.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
    pass.setFilterFieldName("y");
    pass.setFilterLimits(-y,y);
    pass.filter (pointcloud_);
    pass.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
    pass.setFilterFieldName("z");
    pass.setFilterLimits(-z,z);
    pass.filter (pointcloud_);
    if (debug_)
        std::cout<<"Applied passthrough filter: New point size is "<<pointcloud_.size()<<std::endl;
    this->computeNormals();

}
/**
  * @brief apply PCL passthrough filter
  * @param voxel_size size of voxel in meter
  */
void PointCloudDetection::applyVoxelFilter(double voxel_size)
{
    pcl::VoxelGrid<PointRGBT> voxel_grid;
    voxel_grid.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
    voxel_grid.setLeafSize (voxel_size, voxel_size, voxel_size);
    voxel_grid.filter (pointcloud_);
    if (debug_)
        std::cout<<"Applied voxel filter: New point size is "<<pointcloud_.size()<<std::endl;
    this->computeNormals();

}
/**
  * @brief apply PCL outlier filter
  * @param radius radius of filter
  */
void PointCloudDetection::applyOutlierFilter(double radius)
{
    pcl::RadiusOutlierRemoval<PointRGBT> outlier_filter; //remove outlier
    outlier_filter.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
    outlier_filter.setRadiusSearch (radius);
    outlier_filter.setMinNeighborsInRadius(20);
    outlier_filter.filter(pointcloud_);
    if (debug_)
        std::cout<<"Applied outlier filter: New point size is "<<pointcloud_.size()<<std::endl;
    this->computeNormals();
}
/**
  * @brief clusterize member point cloud and returns the clusters
  * @param clusters the vector of clusters
  */
void PointCloudDetection::getClusters(boost::shared_ptr<std::vector<pcl::PointCloud<PointRGBT> > > &clusters)
{

    pcl::search::KdTree<PointRGBT>::Ptr cluster_tree (new pcl::search::KdTree<PointRGBT> ());

    std::vector<pcl::PointIndices> cluster_indices;
    std::vector<pcl::PointIndices>::iterator indices_iter;
    pcl::EuclideanClusterExtraction<PointRGBT> ec;
    pcl::ExtractIndices<PointRGBT> extractor;

    ec.setClusterTolerance(max_cluster_dist_);
    ec.setMinClusterSize(min_cluster_size_);
    ec.setMaxClusterSize(max_cluster_size_);
    ec.setSearchMethod(cluster_tree);
    ec.setInputCloud(boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));
    ec.extract(cluster_indices);

    indices_iter=cluster_indices.begin();

    extractor.setInputCloud(boost::make_shared<pcl::PointCloud<PointRGBT> > (pointcloud_));   

    for (;indices_iter!=cluster_indices.end();indices_iter++){

        pcl::PointCloud<PointRGBT> cluster;
        extractor.setIndices(boost::make_shared<pcl::PointIndices> (*indices_iter));
        extractor.filter(cluster);
        clusters->push_back(cluster);
    }
}

/**
 * Gets the similarity between a cylinder and a given point cloud between 0.0 and 1.0
 * @param input_cloud_ptr the input cloud
 * @return likelihood
 */
double PointCloudDetection::getCylinderLikelihood(
        const pcl::PointCloud<PointRGBT>::Ptr input_cloud_ptr, pcl::ModelCoefficients& coefficients) {


    //The KD tree for the segmentation
    pcl::search::KdTree<PointRGBT>::Ptr tree(new pcl::search::KdTree<PointRGBT>());
    //Structure for normal estimation
    pcl::NormalEstimation<PointRGBT, pcl::Normal> ne; //Normal estimation

    //for the Ransac using Cylinder normals
    pcl::SACSegmentationFromNormals<PointRGBT, pcl::Normal> seg; // the ransac filter

    //the structure to store the cloud normals
    pcl::PointCloud<pcl::Normal> cloud_normals; // the cloud normals

    //for extraction
    pcl::ExtractIndices<PointRGBT> extract; //for point extraction from the filter
    pcl::ExtractIndices<pcl::Normal> extract_normals;

    //all points within a cylinder
    pcl::PointCloud<PointRGBT> cloud_out;

    //the sphere coefficents generated by segmentation
    pcl::PointIndices inliers;


  // Estimate point normals
    ne.setSearchMethod(tree);
    ne.setInputCloud(input_cloud_ptr);
    ne.setKSearch(50);
    ne.compute(cloud_normals);

    // Create the segmentation object for the planar model and set all the parameters
    seg.setOptimizeCoefficients(true);
    seg.setModelType(pcl::SACMODEL_CYLINDER);
    seg.setMethodType(pcl::SAC_RANSAC);

    seg.setNormalDistanceWeight(ransac_normal_dist_weight_);
    seg.setMaxIterations(ransac_iterations_);
    seg.setDistanceThreshold(ransac_dist_threshold_);
    seg.setRadiusLimits(ransac_min_radius_, ransac_max_radius_);

    seg.setInputCloud(input_cloud_ptr);
    seg.setInputNormals(boost::make_shared<pcl::PointCloud<pcl::Normal> >(cloud_normals));
    // Obtain the cylinder inliers and coefficients
    seg.segment(inliers, coefficients);

    if (debug_) PCL_WARN("Cylinder RanSac. Found %d inliers in a cloud of %d points\n",(int) inliers.indices.size(), (int) input_cloud_ptr->size());

    // Extract the inliers from the input cloud



    if (inliers.indices.size() > 0) {
        extract.setInputCloud(input_cloud_ptr);
        extract.setIndices(boost::make_shared<const pcl::PointIndices>(inliers));

        extract_normals.setInputCloud(boost::make_shared<pcl::PointCloud<pcl::Normal> >(cloud_normals));
        extract_normals.setIndices(boost::make_shared<const pcl::PointIndices>(inliers));

        extract.setNegative(false);
        extract.filter(cloud_out);        

        return static_cast<double>(cloud_out.points.size())/input_cloud_ptr->size();

    } else {
        return 0.0;
    }
}

/**
 *
 * @param input_cloud_ptr The input point cloud
 * @param max_pt the maximum point
 * @param min_pt the minimum point
 * @param coefficients the coefficents coming from the segmentation
 * @return the height of the cylinder
 */
double PointCloudDetection::getCylinderHeight(const pcl::PointCloud<PointRGBT>::Ptr input_cloud_ptr, PointT &max_pt, PointT &min_pt, const pcl::ModelCoefficients coefficients){

    Eigen::Vector3f orientation(coefficients.values[3],coefficients.values[4],coefficients.values[5]);

    Eigen::Vector3f pointOnAxis(coefficients.values[0],coefficients.values[1],coefficients.values[2]);
    Eigen::Vector3f max_point;
    Eigen::Vector3f min_point;
    Eigen::Vector3f pointTranslated;

    pointTranslated=input_cloud_ptr->points[0].getVector3fMap()-pointOnAxis;

    double min=(pointTranslated.dot(orientation))/orientation.norm();
    double max=(pointTranslated.dot(orientation))/orientation.norm();
    min_point=pointOnAxis+min*orientation;
    max_point=pointOnAxis+max*orientation;

    BOOST_FOREACH(const PointRGBT& pt, input_cloud_ptr->points){

        //Translate all points in the direction of the axis point
        pointTranslated=pt.getVector3fMap()-pointOnAxis;

        //Project all points on the unit vector and comparing the scalar product
        if (pointTranslated.dot(orientation)<min){
            min=(pointTranslated.dot(orientation))/orientation.norm();
            //retransform the point to the original axis
            min_point=pointOnAxis+min*orientation;
        }
        if (pointTranslated.dot(orientation)>max){
            max=(pointTranslated.dot(orientation))/orientation.norm();
            //retransform the point to the original axis
            max_point=pointOnAxis+max*orientation;
        }
    }

    //Conversion back to Point Type
    max_pt.x=max_point[0];
    max_pt.y=max_point[1];
    max_pt.z=max_point[2];

    min_pt.x=min_point[0];
    min_pt.y=min_point[1];
    min_pt.z=min_point[2];

    return max-min;

}

/**
 * @brief gets the model score given the extraced feautres and the globally defined model for cylinders
 * @param detectedObject the detected object
 * @return the likelihood between 0,1
 */
double PointCloudDetection::getCylinderScore(object_detection::Cylinder detectedObject){

    double likelihood=0.0;

    //set up two normal distributions for likelihood calculation
    boost::math::normal_distribution<double> heightND(0,0.05);
    boost::math::normal_distribution<double> diameterND(0,0.07);
    //assume the objects are on ground level and not flying
    boost::math::normal_distribution<double> groundND(0,0.20);


    double heightDiff=cylinderModel_.height-detectedObject.height;
    double diameterDiff=cylinderModel_.diameter-detectedObject.diameter;
    double GNDDiff=detectedObject.position.z()-ground_level_;

    PCL_WARN("Object is %f above/below ground\n",GNDDiff);

    likelihood=pdf(heightND,heightDiff)*pdf(diameterND,diameterDiff)*pdf(groundND,fabs(GNDDiff-config.groundDistance))/
            (pdf(diameterND,0)*pdf(heightND,0)*pdf(groundND,0));


    return likelihood;


}
/**
 * @brief gets the model score given the extraced feautres and the globally defined model for boxes
 * @param detectedObject the detected object
 * @return the likelihood between 0,1
 */
double PointCloudDetection::getBoxScore(object_detection::Box detectedObject, const double& rectangularness){

    double likelihood=0.0;

    //set up a normal distributions for the given edge length of the box
    boost::math::normal_distribution<double> edgeLengthND(0,0.07);
    boost::math::normal_distribution<double> shapeND(0,0.50);
    //assume the objects are on ground level and not flying
    boost::math::normal_distribution<double> groundND(0,0.20);


    double widthDiff=boxModel_.dimensions[0]-detectedObject.dimensions[0];
    double lengthDiff=boxModel_.dimensions[1]-detectedObject.dimensions[1];
    double shapeDiff=1.0-rectangularness;
    double GNDDiff=detectedObject.position.z()-ground_level_;

    likelihood=pdf(edgeLengthND,widthDiff)*pdf(edgeLengthND,lengthDiff)*pdf(shapeND,shapeDiff)*pdf(groundND,fabs(GNDDiff-config.groundDistance))/
            (pdf(edgeLengthND,0)*pdf(edgeLengthND,0)*pdf(shapeND,0)*pdf(groundND,0));


    return likelihood;

}

/**
 * @brief Method for detecting the base in the point cloud as an compound object
 * @param the base as a box model
 * @return found a base
 */
bool PointCloudDetection::getBase(object_detection::Box& box)
{

    std::vector<pcl::PointIndices>::iterator iter;
    std::vector <pcl::PointIndices> clusters;

    this->extractPlanes(clusters);

    base_.clear();

    for (iter=clusters.begin();iter!=clusters.end();iter++){

        //object_detection::Plane plane(
        pcl::PointCloud<PointRGBT> cloud_plane(pointcloud_,iter->indices);
        segmentation::Plane plane;
        for (pcl::PointCloud<PointRGBT>::iterator pIter=cloud_plane.begin();pIter!=cloud_plane.end();pIter++){
            plane.addPoint(segmentation::Shared_Point(new segmentation::Point((double)pIter->x,(double)pIter->y,(double)pIter->z)));
        }

        //get the feautures of the plane

        base::Pose pose;
        pose.position=plane.getCoG();
        pose.orientation=plane.getQuaternion();


        //add component to the potential base object
        object_detection::Plane component(plane.getRectangularness(),
                                          plane.getSizeOfPlane(),
                                          plane.getMaxExpansion().width,
                                          plane.getMaxExpansion().height,                                          
                                          pose,
                                          plane.getNormalVector(),
                                          plane.getSpatialExpansion());

        component.setPoints(cloud_plane.makeShared());
        base_.addComponent(component);        
    }

    //try to assemble the object based on the components    

    if (base_.assembleObject()){
        box.position=base_.getPosition();
        box.orientation=base_.getOrientation();        
        box.dimensions=base_.getDimensions();
        box.likelihood=base_.getLikelihood();
        return true;
    }

    else
        return false;
}

/**
 * @brief Method for detecting the battery pack in the point cloud as an compound object
 * @param the base as a box model
 * @return found a base
 */
bool PointCloudDetection::getBattery(object_detection::Box& box){


    std::vector<pcl::PointIndices>::iterator iter;
    std::vector <pcl::PointIndices> clusters;

    this->extractPlanes(clusters);

    battery_.clear();

    for (iter=clusters.begin();iter!=clusters.end();iter++){

        //object_detection::Plane plane(
        pcl::PointCloud<PointRGBT> cloud_plane(pointcloud_,iter->indices);
        segmentation::Plane plane;
        for (pcl::PointCloud<PointRGBT>::iterator pIter=cloud_plane.begin();pIter!=cloud_plane.end();pIter++){
            plane.addPoint(segmentation::Shared_Point(new segmentation::Point((double)pIter->x,(double)pIter->y,(double)pIter->z)));
        }

        //get the feautures of the plane

        base::Pose pose;
        pose.position=plane.getCoG();
        pose.orientation=plane.getQuaternion();

        //add component to the potential base object
        object_detection::Plane component(plane.getRectangularness(),
                                          plane.getSizeOfPlane(),
                                          plane.getMaxExpansion().width,
                                          plane.getMaxExpansion().height,                                          
                                          pose,
                                          plane.getNormalVector(),
                                          plane.getSpatialExpansion());

        component.setPoints(cloud_plane.makeShared());
        battery_.addComponent(component);
    }


    //try to assemble the object based on the components

    if (battery_.assembleObject()){
        box.position=battery_.getPosition();
        box.orientation=battery_.getOrientation();
        box.dimensions=battery_.getDimensions();
        box.likelihood=battery_.getLikelihood();
        return true;
    }
    else
        return false;

}
/**
 * @brief Method for extracting planes from the pointcloud
 * @param clusters vector of point indices
 */
void PointCloudDetection::extractPlanes(std::vector <pcl::PointIndices>& clusters)
{
    //use region growing, more robust than RanSaC
    pcl::RegionGrowing<PointRGBT, pcl::Normal> reg;
    pcl::search::Search<PointRGBT>::Ptr tree = boost::shared_ptr<pcl::search::Search<PointRGBT> > (new pcl::search::KdTree<PointRGBT>);

    //region growing to get planes
    reg.setMinClusterSize (min_cluster_size_);
    reg.setMaxClusterSize (max_cluster_size_);
    reg.setSearchMethod (tree);
    reg.setNumberOfNeighbours (30);

    reg.setInputCloud (boost::make_shared<pcl::PointCloud<PointRGBT> >(pointcloud_));
    reg.setInputNormals (boost::make_shared<pcl::PointCloud<pcl::Normal> >(cloud_normals_));
    reg.setSmoothnessThreshold (config.smoothness/ 180.0 * M_PI);
    reg.setCurvatureThreshold (config.curvature);

    reg.extract (clusters);

}

/**
 * @brief returns the likelihood of the component matching the color
 * @param colorA the 1st color to check
 * @param colorB the 2nd color to check
 * @return percentage of color match [0:1]
 */
double PointCloudDetection::matchesColor(const object_detection::Color colorA,const object_detection::Color colorB){

    double value=0.0;

    //set up a normal distributions for the given edge length of the box
    boost::math::normal_distribution<double> redND(0,0.10);
    boost::math::normal_distribution<double> greenND(0,0.10);
    boost::math::normal_distribution<double> blueND(0,0.10);


    double redDiff=colorA.r-colorB.r;
    double greenDiff=colorA.g-colorB.g;
    double blueDiff=colorA.b-colorB.b;

    value=pdf(redND,redDiff)*pdf(greenND,greenDiff)*pdf(blueND,blueDiff)/
            (pdf(redND,0)*pdf(greenND,0)*pdf(blueND,0));


    return value;

}


/** @brief gets the score of how parallel two axis are to each other in an interval [0,1]
 *  @param axis the axis to compare
 *  @return parallel score
 */
double PointCloudDetection::isVertical(base::Vector3d axis)
{
    base::Vector3d vert(0,0,1);
    double value = fabs(1-fabs(vert.dot(axis)-(vert.norm()*axis.norm())));

    if (value<0.5) //cut off
        return 0.0;
    else
        return value;

}


