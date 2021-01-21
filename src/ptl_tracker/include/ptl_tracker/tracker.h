#pragma once
#include <iostream>
#include <mutex>
#include <string>

//ros
#include "ros/ros.h"
#include <tf2_ros/transform_listener.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include "cv_bridge/cv_bridge.h"
#include <image_transport/image_transport.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <pcl_ros/point_cloud.h>

//ros msg
#include "std_msgs/UInt16MultiArray.h"
#include "sensor_msgs/Image.h"
#include "sensor_msgs/CompressedImage.h"
#include <geometry_msgs/TransformStamped.h>
#include <geometry_msgs/Point.h>
#include <sensor_msgs/PointCloud2.h>
#include <visualization_msgs/Marker.h>

//opentracker
#include "opentracker/kcf/kcftracker.hpp"

#include "ptl_tracker/local_object.h"
#include "ptl_tracker/association_type.hpp"
#include "ptl_tracker/point_cloud_processor.h"
#include "ptl_msgs/ImageBlock.h"
#include "ptl_msgs/ReidInfo.h"

namespace ptl
{
    namespace tracker
    {
        class TrackerInterface
        {
        public:
            TrackerInterface(ros::NodeHandle *n);

        private:
            void detector_result_callback(const ptl_msgs::ImageBlockPtr &msg);

            void tracker_callback(const sensor_msgs::ImageConstPtr &msg);
            void track_and_locate_callback(const sensor_msgs::ImageConstPtr &msg_img, const sensor_msgs::PointCloud2ConstPtr &msg_pc);

            void tracker_callback_compressed(const sensor_msgs::CompressedImageConstPtr &msg);
            void track_and_locate_callback_compressed(const sensor_msgs::CompressedImageConstPtr &msg_img, const sensor_msgs::PointCloud2ConstPtr &msg_pc);

            void reid_callback(const ptl_msgs::ReidInfo &msg);
            void load_config(ros::NodeHandle *n);
            bool update_local_database(LocalObject &local_object, const cv::Mat &img_block);
            bool update_local_database(std::vector<LocalObject>::iterator local_object, const cv::Mat &img_block);
            void match_between_2d_and_3d(const pcl::PointCloud<pcl::PointXYZI>::Ptr pc, const ros::Time &ros_pc_time);
            void get_tf();
            void update_tracker_pos_marker_visualization();
            void update_overlap_flag();

            //do segementation by reprojection
            pcl::PointCloud<pcl::PointXYZI> point_cloud_segementation(const pcl::PointCloud<pcl::PointXYZI>::Ptr pc, const cv::Rect2d &bbox);

        public:
            std::vector<LocalObject> local_objects_list;

        private:
            bool blur_detection(cv::Mat img);

            int id;
            ros::NodeHandle *nh_;
            ros::Publisher m_track_vis_pub, m_track_to_reid_pub, m_track_marker_pub;
            ros::Publisher m_pc_filtered_debug, m_pc_cluster_debug;
            ros::Subscriber m_detector_sub, m_data_sub, m_reid_sub;
            std::mutex mtx;
            struct ReidInfo reid_infos;
            tf2_ros::Buffer tf_buffer;
            tf2_ros::TransformListener *tf_listener;
            message_filters::Subscriber<sensor_msgs::CompressedImage> m_compressed_image_sub;
            message_filters::Subscriber<sensor_msgs::Image> m_image_sub;
            message_filters::Subscriber<sensor_msgs::PointCloud2> m_lidar_sub;

            typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::PointCloud2> MySyncPolicy;
            message_filters::Synchronizer<MySyncPolicy> *sync;

            typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::CompressedImage, sensor_msgs::PointCloud2> MySyncPolicyCompressed;
            message_filters::Synchronizer<MySyncPolicyCompressed> *sync_compressed;

            geometry_msgs::TransformStamped lidar2camera, lidar2map, camera2map;

            //param
            std::string lidar_topic, camera_topic;
            std::string map_frame, lidar_frame, camera_frame;
            bool use_compressed_image = true;
            bool use_lidar = false;
            bool enable_pcp_vis = true;
            int track_fail_timeout_tick = 30;
            double bbox_overlap_ratio_threshold = 0.5;
            int track_to_reid_bbox_margin = 10;
            float height_width_ratio_min = 1.0;
            float height_width_ratio_max = 3.0;
            float blur_detection_threshold = 160.0;
            float record_interval = 0.1;
            int batch_num_min = 3;

            int detector_update_timeout_tick = 10;
            int detector_bbox_padding = 10;
            float reid_match_threshold = 200;
            double reid_match_bbox_dis = 30;
            double reid_match_bbox_size_diff = 30;

            int match_centroid_padding = 20;
            TrackerParam tracker_param;
            PointCloudProcessorParam pcp_param;
            CameraIntrinsic camera_intrinsic;
            KalmanFilterParam kf_param;
            KalmanFilter3dParam kf3d_param;
        };
    } // namespace tracker

} // namespace ptl
