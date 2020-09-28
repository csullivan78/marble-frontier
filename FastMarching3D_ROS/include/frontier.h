#include <vector>
#include "math.h"
#include <iostream>
#include <omp.h>
// pcl libraries
#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/filters/frustum_culling.h>
#include <pcl/features/normal_3d.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
// pcl ROS
#include <pcl_conversions/pcl_conversions.h>
// Octomap libaries
#include <octomap/octomap.h>
#include <octomap/ColorOcTree.h>
#include <octomap_msgs/Octomap.h>
#include <octomap_msgs/conversions.h>
// local packages
#include <mapGrid3D.h>

struct FrontierVoxel {
  pcl::PointXYZ position;
  pcl::Normal normal;
  int cluster = 0;
  int group = 0;
};

struct SeenOccupiedVoxel {
  bool seen = false;
  bool occupied = false;
};

struct Frontier {
  std::vector<FrontierVoxel> list;
  MapGrid3D<std::pair<bool,int>> map; // Stores whether or not a voxel is a frontier and the vector index of the frontier voxel at each x,y,z location
};

Frontier ConvertPointCloudToFrontier(pcl::PointCloud<pcl::PointXYZ>::Ptr frontierList, MapGrid3D<std::pair<bool,bool>>* seen)
{
  Frontier frontier;
  frontier.map = MapGrid3D<std::pair<bool,int>>(seen->voxelSize, seen->size, seen->minBounds);
  frontier.map.SetAll(std::make_pair(false,-1));
  for (int i=0; i<frontierList->points.size(); i++) {
    FrontierVoxel v;
    v.position = frontierList->points[i];
    frontier.list.push_back(v);
    std::pair<bool,int> vMap;
    vMap.first = true;
    vMap.second = frontier.list.size();
    frontier.map.SetVoxel(v.position.x, v.position.y, v.position.z, vMap);
  }
  return frontier;
}

Frontier ConvertPointCloudToFrontier(pcl::PointCloud<pcl::PointNormal>::Ptr frontierList, MapGrid3D<std::pair<bool,bool>>* seen)
{
  Frontier frontier;
  frontier.map = MapGrid3D<std::pair<bool,int>>(seen->voxelSize, seen->size, seen->minBounds);
  frontier.map.SetAll(std::make_pair(false,-1));
  for (int i=0; i<frontierList->points.size(); i++) {
    pcl::PointNormal p = frontierList->points[i];
    FrontierVoxel v;
    v.position.x = p.x; v.position.y = p.y; v.position.z = p.z;
    v.normal.normal_x = p.normal_x; v.normal.normal_y = p.normal_y; v.normal.normal_z = p.normal_z;
    frontier.list.push_back(v);
    std::pair<bool,int> vMap;
    vMap.first = true;
    vMap.second = frontier.list.size();
    frontier.map.SetVoxel(v.position.x, v.position.y, v.position.z, vMap);
  }
  return frontier;
}

Frontier ConvertPointCloudToFrontier(pcl::PointCloud<pcl::PointNormal>::Ptr frontierList, std::vector<pcl::PointIndices> frontierClusterIndices, MapGrid3D<std::pair<bool,bool>>* seen)
{
  Frontier frontier;
  frontier.map = MapGrid3D<std::pair<bool,int>>(seen->voxelSize, seen->size, seen->minBounds);
  frontier.map.SetAll(std::make_pair(false,-1));
  for (int cluster=0; cluster<frontierClusterIndices.size(); cluster++) {
    for (int i=0; i<frontierClusterIndices[cluster].indices.size(); i++) {
      int idx = frontierClusterIndices[cluster].indices[i];
      pcl::PointNormal p = frontierList->points[idx];
      FrontierVoxel v;
      v.position.x = p.x; v.position.y = p.y; v.position.z = p.z;
      v.normal.normal_x = p.normal_x; v.normal.normal_y = p.normal_y; v.normal.normal_z = p.normal_z;
      v.cluster = cluster;
      frontier.list.push_back(v);
      std::pair<bool,int> vMap;
      vMap.first = true;
      vMap.second = frontier.list.size();
      Point query{v.position.x, v.position.y, v.position.z};
      if (frontier.map._CheckVoxelPositionInBounds(query)) { // Got a nan for x value so this is a band-aid until I find out why
        frontier.map.SetVoxel(v.position.x, v.position.y, v.position.z, vMap);
      }
    }
  }
  return frontier;
}

void FilterFrontierByNormal(pcl::PointCloud<pcl::PointXYZ>::Ptr frontierList, pcl::PointCloud<pcl::PointNormal>::Ptr frontierListNormalFiltered, float normalZMax, float radius)
{
  // Create cloud pointer to store the removed points
  pcl::PointCloud<pcl::PointNormal>::Ptr normalCloud(new pcl::PointCloud<pcl::PointNormal>);

  // Creating the KdTree object for the search method of the extraction
  pcl::search::KdTree<pcl::PointXYZ>::Ptr kdtree(new pcl::search::KdTree<pcl::PointXYZ>);
  kdtree->setInputCloud(frontierList);

  // Initialize euclidean cluster extraction object
  pcl::NormalEstimationOMP<pcl::PointXYZ, pcl::PointNormal> ne;
  ne.setSearchMethod(kdtree);
  ne.setInputCloud(frontierList);
  ne.setRadiusSearch(radius);
  ne.compute(*normalCloud);

  // Keep all points with a normal z-component less than normalZMax
  frontierListNormalFiltered->points.clear();
  for (int i=0; i<normalCloud->points.size(); i++) {
    pcl::PointNormal query = normalCloud->points[i];
    if (std::abs(query.normal_z) <= normalZMax) {
      query.x = frontierList->points[i].x;
      query.y = frontierList->points[i].y;
      query.z = frontierList->points[i].z;
      frontierListNormalFiltered->points.push_back(query);
    }
  }
  return;
}

void FilterFrontierByCluster(pcl::PointCloud<pcl::PointNormal>::Ptr frontierList, pcl::PointCloud<pcl::PointNormal>::Ptr frontierListFiltered, 
std::vector<pcl::PointIndices> &frontierClusterIndices, float clusterDistance, int minClusterSize)
{
  // Creating the KdTree object for the search method of the extraction
  pcl::search::KdTree<pcl::PointNormal>::Ptr kdtree(new pcl::search::KdTree<pcl::PointNormal>);
  kdtree->setInputCloud(frontierList);

  // Extract clusters
  pcl::EuclideanClusterExtraction<pcl::PointNormal> ec;
  ec.setClusterTolerance(clusterDistance); // Clusters must be made of contiguous sections of frontier (within sqrt(2)*voxel_size of each other)
  ec.setMinClusterSize(minClusterSize); // Cluster must be at least 15 voxels in size
  ec.setSearchMethod(kdtree);
  ec.setInputCloud(frontierList);
  ec.extract(frontierClusterIndices);

  // Add clusters to output frontier list
  frontierListFiltered->points.clear();
  for (int cluster=0; cluster<frontierClusterIndices.size(); cluster++) {
    for (int i=0; i<frontierClusterIndices[cluster].indices.size(); i++) {
      int idx = frontierClusterIndices[cluster].indices[i];
      frontierListFiltered->points.push_back(frontierList->points[idx]);
      frontierClusterIndices[cluster].indices[i] = frontierListFiltered->points.size();
    }
  }
  return;
}

void ConvertOctomapToSeenOccGrid(octomap::OcTree* map, MapGrid3D<std::pair<bool,bool>>* seenOccGrid)
{
  // Initialize MapGrid3D object of the appropriate size
  double xMin, yMin, zMin, xMax, yMax, zMax;
  seenOccGrid->voxelSize = map->getResolution();
  map->getMetricMin(xMin, yMin, zMin);
  map->getMetricMax(xMax, yMax, zMax);
  float minBounds[3] = {xMin - 1.5*seenOccGrid->voxelSize, yMin - 1.5*seenOccGrid->voxelSize, zMin - 1.5*seenOccGrid->voxelSize};
  float maxBounds[3] = {xMax + 1.5*seenOccGrid->voxelSize, yMax + 1.5*seenOccGrid->voxelSize, zMax + 1.5*seenOccGrid->voxelSize};
  seenOccGrid->Reset(seenOccGrid->voxelSize, minBounds, maxBounds, std::make_pair(false, false));
  map->expand();

  clock_t tStart = clock();
  for(octomap::OcTree::leaf_iterator it = map->begin_leafs(),
  end=map->end_leafs(); it!=end; ++it) {
    seenOccGrid->SetVoxel(it.getX(), it.getY(), it.getZ(), std::make_pair(true, (it->getValue()>0.0)));
  }
  ROS_INFO("Copying data from Octomap took: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);
  return;
}

void GetPointCloudBounds(pcl::PointCloud<pcl::PointXYZI>::Ptr cloud, float min[3], float max[3])
{
  Point boundsMin{0.0, 0.0, 0.0};
  Point boundsMax{0.0, 0.0, 0.0};

  // Get the xyz extents of the PCL by running one loop through the data
  for (int i=0; i<cloud->points.size(); i++) {
    pcl::PointXYZI query = cloud->points[i];
    if (i == 0) {
      boundsMin.x = (double)query.x; boundsMin.y = (double)query.y; boundsMin.z = (double)query.z;
      boundsMax.x = (double)query.x; boundsMax.y = (double)query.y; boundsMax.z = (double)query.z;
      continue;
    }
    if (query.x < boundsMin.x) boundsMin.x = (double)query.x;
    if (query.y < boundsMin.y) boundsMin.y = (double)query.y;
    if (query.z < boundsMin.z) boundsMin.z = (double)query.z;
    if (query.x > boundsMax.x) boundsMax.x = (double)query.x;
    if (query.y > boundsMax.y) boundsMax.y = (double)query.y;
    if (query.z > boundsMax.z) boundsMax.z = (double)query.z;
  }

  min[0] = boundsMin.x; min[1] = boundsMin.y; min[2] = boundsMin.z;
  max[0] = boundsMax.x; max[1] = boundsMax.y; max[2] = boundsMax.z;
  return;
}

void ConvertPointCloudToSeenOccGrid(pcl::PointCloud<pcl::PointXYZI>::Ptr edt, MapGrid3D<std::pair<bool,bool>>* seenOccGrid, float minDistanceFree)
{
  float minBounds[3];
  float maxBounds[3];
  GetPointCloudBounds(edt, minBounds, maxBounds);
  seenOccGrid->Reset(seenOccGrid->voxelSize, minBounds, maxBounds, std::make_pair(false,false));

  for (int i=0; i<edt->points.size(); i++) {
    pcl::PointXYZI query = edt->points[i];
    seenOccGrid->SetVoxel(query.x, query.y, query.z, std::make_pair(true, (query.intensity<=minDistanceFree)));
  }

  return;
}

bool CheckFrontier(int voxelID, std::vector<int> neighbors, MapGrid3D<std::pair<bool, bool>>* seenOccGrid) {
  // The voxel is free and seen, check the neighbors. If any are unseen, return true.
  for (int neighbor=0; neighbor<neighbors.size(); neighbor++) {
    if (!seenOccGrid->voxels[voxelID+neighbors[neighbor]].first) return true;
  }
  return false;
}

bool CheckFrontierGroundPlane(int voxelID, std::vector<int> neighbors, MapGrid3D<std::pair<bool, bool>>* seenOccGrid) {
  // This is a function check for frontiers of ground plane maps with a voxel thickness of 2 or more.
  // Most voxels aren't going to be frontier, so lets eliminate those first
  int unseenCount = 0;
  for (int neighbor=0; neighbor<4; neighbor++) { // Only check x and y neighbors
    // Check if neighbor is occupied, if so, eliminate this as a candidate
    std::pair<bool,bool> voxel = seenOccGrid->voxels[voxelID + neighbors[neighbor]];
    if (voxel.second) return false;
    // If at least two neighbors are unseen and none are occupied, return true.
    if (!voxel.first) unseenCount++;
  }
  if (unseenCount >= 1) return true;
  else return false;
}

bool CalculateFrontierRawPCL(MapGrid3D<std::pair<bool, bool>>* seenOccGrid, pcl::PointCloud<pcl::PointXYZ>::Ptr frontier)
{
  frontier->points.clear();
  std::vector<int> neighbor_deltas;
  neighbor_deltas.push_back(1);
  neighbor_deltas.push_back(-1);
  neighbor_deltas.push_back(seenOccGrid->size.x);
  neighbor_deltas.push_back(-seenOccGrid->size.x);
  neighbor_deltas.push_back(seenOccGrid->size.x*seenOccGrid->size.y);
  neighbor_deltas.push_back(-seenOccGrid->size.x*seenOccGrid->size.y);
  int total_map_size = seenOccGrid->voxels.size();
  // Check all the map cells for frontier voxels
  for (int i=0; i<seenOccGrid->voxels.size(); i++){
    // Check if the voxel is seen and free (not occupied)
    if (seenOccGrid->voxels[i].first && !seenOccGrid->voxels[i].second) {
      // This section will segfault if any seen and free cells are on the border of the map.
      // Be sure to pad the seenOccGrid with at least one unseen voxel to prevent this.
      if (CheckFrontier(i, neighbor_deltas, seenOccGrid)) {
        Point query = seenOccGrid->_ConvertIndexToPosition(i);
        pcl::PointXYZ p{query.x, query.y, query.z};
        frontier->points.push_back(p);
      }
    }
  }
}

bool CalculateFrontierRawPCLGroundPlane(MapGrid3D<std::pair<bool, bool>>* seenOccGrid, pcl::PointCloud<pcl::PointXYZ>::Ptr frontier)
{
  frontier->points.clear();
  std::vector<int> neighbor_deltas;
  neighbor_deltas.push_back(1);
  neighbor_deltas.push_back(-1);
  neighbor_deltas.push_back(seenOccGrid->size.x);
  neighbor_deltas.push_back(-seenOccGrid->size.x);
  neighbor_deltas.push_back(seenOccGrid->size.x*seenOccGrid->size.y);
  neighbor_deltas.push_back(-seenOccGrid->size.x*seenOccGrid->size.y);
  int total_map_size = seenOccGrid->voxels.size();
  // Check all the map cells for frontier voxels
  for (int i=0; i<seenOccGrid->voxels.size(); i++){
    // Check if the voxel is seen and free (not occupied)
    if (seenOccGrid->voxels[i].first && !seenOccGrid->voxels[i].second) {
      // This section will segfault if any seen and free cells are on the border of the map.
      // Be sure to pad the seenOccGrid with at least one unseen voxel to prevent this.
      if (CheckFrontierGroundPlane(i, neighbor_deltas, seenOccGrid)) {
        Point query = seenOccGrid->_ConvertIndexToPosition(i);
        pcl::PointXYZ p{query.x, query.y, query.z};
        frontier->points.push_back(p);
      }
    }
  }
}

Frontier CalculateFrontier(octomap::OcTree* map, bool normalFilterOn=true, bool clusteringOn=true, float minNormalZ=0.4, int minClusterSize=50)
{
  clock_t tStart = clock();
  MapGrid3D<std::pair<bool,bool>> seenOcc;
  ConvertOctomapToSeenOccGrid(map, &seenOcc);
  ROS_INFO("Octomap converted to seenOcc GridMap in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  // Determines which voxels are frontier by rule:
  // Frontier Voxel - A seen and free voxel adjacent to an unseen voxel
  tStart = clock();
  pcl::PointCloud<pcl::PointXYZ>::Ptr frontierCloudRaw(new pcl::PointCloud<pcl::PointXYZ>);
  CalculateFrontierRawPCL(&seenOcc, frontierCloudRaw);
  ROS_INFO("Raw frontier computed (fastmode) in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);


  // Calculate frontier normal vectors and filter according to the z-component
  tStart = clock();
  pcl::PointCloud<pcl::PointNormal>::Ptr frontierCloudFilteredNormals(new pcl::PointCloud<pcl::PointNormal>);
  FilterFrontierByNormal(frontierCloudRaw, frontierCloudFilteredNormals, 2.1*seenOcc.voxelSize, minNormalZ);
  ROS_INFO("Normals filtered in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  // Cluster frontier voxels based on contiguity
  tStart = clock();
  pcl::PointCloud<pcl::PointNormal>::Ptr frontierCloudFilteredClustered(new pcl::PointCloud<pcl::PointNormal>);
  std::vector<pcl::PointIndices> frontierClusterIndices;
  FilterFrontierByCluster(frontierCloudFilteredNormals, frontierCloudFilteredClustered, frontierClusterIndices, 1.5*seenOcc.voxelSize, minClusterSize);
  ROS_INFO("Clusters calculated in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  // Convert PointCloud to Frontier struct
  tStart = clock();
  Frontier frontier = ConvertPointCloudToFrontier(frontierCloudFilteredClustered, frontierClusterIndices, &seenOcc);
  ROS_INFO("PointCloud converted to Frontier struct in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  return frontier;
}

Frontier CalculateFrontier(pcl::PointCloud<pcl::PointXYZI>::Ptr map, float voxelSize, bool normalFilterOn=true, bool clusteringOn=true, float minNormalZ=0.4, int minClusterSize=50, bool groundMap=false)
{
  clock_t tStart = clock();
  MapGrid3D<std::pair<bool,bool>> seenOcc;
  seenOcc.voxelSize = voxelSize;
  ConvertPointCloudToSeenOccGrid(map, &seenOcc, 2.1*voxelSize);
  ROS_INFO("EDT converted to seenOcc GridMap in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  // Determines which voxels are frontier by rule:
  // Frontier Voxel - A seen and free voxel adjacent to an unseen voxel
  tStart = clock();
  pcl::PointCloud<pcl::PointXYZ>::Ptr frontierCloudRaw(new pcl::PointCloud<pcl::PointXYZ>);
  if (groundMap) CalculateFrontierRawPCLGroundPlane(&seenOcc, frontierCloudRaw);
  else CalculateFrontierRawPCL(&seenOcc, frontierCloudRaw);
  ROS_INFO("Raw frontier computed (fastmode) in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);


  // Calculate frontier normal vectors and filter according to the z-component
  tStart = clock();
  pcl::PointCloud<pcl::PointNormal>::Ptr frontierCloudFilteredNormals(new pcl::PointCloud<pcl::PointNormal>);
  if (normalFilterOn) FilterFrontierByNormal(frontierCloudRaw, frontierCloudFilteredNormals, 2.1*seenOcc.voxelSize, minNormalZ);
  else {
    for (int i=0; i<frontierCloudRaw->points.size(); i++) {
      pcl::PointNormal query;
      query.x = frontierCloudRaw->points[i].x; query.y = frontierCloudRaw->points[i].y; query.z = frontierCloudRaw->points[i].z;
      frontierCloudFilteredNormals->points.push_back(query);
    }
  }
  ROS_INFO("Normals filtered in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  // Cluster frontier voxels based on contiguity
  tStart = clock();
  pcl::PointCloud<pcl::PointNormal>::Ptr frontierCloudFilteredClustered(new pcl::PointCloud<pcl::PointNormal>);
  std::vector<pcl::PointIndices> frontierClusterIndices;
  if (clusteringOn) FilterFrontierByCluster(frontierCloudFilteredNormals, frontierCloudFilteredClustered, frontierClusterIndices, 1.5*seenOcc.voxelSize, minClusterSize);
  else {
    pcl::PointIndices ids;
    for (int i=0; i<frontierCloudFilteredNormals->points.size(); i++) {
      frontierCloudFilteredClustered->points.push_back(frontierCloudFilteredNormals->points[i]);
      ids.indices.push_back(i);
    }
    frontierClusterIndices.push_back(ids);
  }
  ROS_INFO("Clusters calculated in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  // Convert PointCloud to Frontier struct
  tStart = clock();
  Frontier frontier = ConvertPointCloudToFrontier(frontierCloudFilteredClustered, frontierClusterIndices, &seenOcc);
  ROS_INFO("PointCloud converted to Frontier struct in: %.5fs", (double)(clock() - tStart)/CLOCKS_PER_SEC);

  return frontier;
}

sensor_msgs::PointCloud2 ConvertFrontierToROSMsg(Frontier frontier)
{
  pcl::PointCloud<pcl::PointXYZ>::Ptr frontierList(new pcl::PointCloud<pcl::PointXYZ>);
  for (int i=0; i<frontier.list.size(); i++){
    frontierList->points.push_back(frontier.list[i].position);
  }
  sensor_msgs::PointCloud2 msg;
  pcl::toROSMsg(*frontierList, msg);
  return msg;
}