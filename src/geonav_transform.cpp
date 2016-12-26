#include "geonav_transform/geonav_transform.h"
#include "geonav_transform/navsat_conversions.h"
#include "geonav_transform/geonav_utilities.h"

#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <XmlRpcException.h>

#include <string>

namespace GeonavTransform
{
GeonavTransform::GeonavTransform() :
  // Initialize attributes
  broadcast_utm_transform_(false),
  nav_frame_id_(""),
  zero_altitude_(false),
  world_frame_id_("odom"),
  base_link_frame_id_("base_link"),
  utm_zone_(""),
  tf_listener_(tf_buffer_)
{
  transform_utm2nav_covariance_.resize(POSE_SIZE, POSE_SIZE);
}

GeonavTransform::~GeonavTransform()
{
}

void GeonavTransform::run()
{
  ros::Time::init();
  
  double frequency = 10.0;
  double delay = 0.0;
  
  ros::NodeHandle nh;
  ros::NodeHandle nh_priv("~");
  
  // Load ROS parameters
  nh_priv.param("frequency", frequency, 10.0);
  nh_priv.param("broadcast_utm_transform", broadcast_utm_transform_, false);
  nh_priv.param("zero_altitude", zero_altitude_, false);

  // Datum parameter - required
  double datum_lat;
  double datum_lon;
  double datum_yaw;
  if (! nh_priv.hasParam("datum"))
  {
    ROS_ERROR("ERROR <datum> parameter is not supplied in "
	      "geonav_transform configuration");
    ROS_ERROR("Setting to 0,0,0 which is non-ideal!");
    datum_lat = datum_lon = datum_yaw = 0.0;
  }
  else
  {
    XmlRpc::XmlRpcValue datum_config;
    try
    {
      nh_priv.getParam("datum", datum_config);
      
      /* Handle datum specification. 
	 Users should always specify a baseLinkFrameId_ in the
	 datum config, but we had a release where it wasn't used, 
	 so we'll maintain compatibility.*/
      ROS_ASSERT(datum_config.getType() == XmlRpc::XmlRpcValue::TypeArray);
      ROS_ASSERT(datum_config.size() >= 3);
      
      if (datum_config.size() > 3)
      {
	ROS_WARN_STREAM("Deprecated datum parameter configuration detected. "
			"Only the first three parameters "
			"(latitude, longitude, yaw) will be used. frame_ids "
			"will be derived from odometry and navsat inputs.");
      }
      // Parse the data spec.
      std::ostringstream ostr;
      ostr << datum_config[0] << " " << datum_config[1] << " " << datum_config[2];
      std::istringstream istr(ostr.str());
      istr >> datum_lat >> datum_lon >> datum_yaw;
    }
    catch (XmlRpc::XmlRpcException &e)
    {
      ROS_ERROR_STREAM("ERROR datum config: " << e.getMessage() <<
		       " for geonav_transform (type: " 
		       << datum_config.getType() << ")");
      ROS_ERROR("Setting to 0,0,0 which is non-ideal!");
      datum_lat = datum_lon = datum_yaw = 0.0;
    }
    
    // Tell everyone we are ignoring yaw in the datum
    if (fabs(datum_yaw) > 0.01)
    {
      ROS_WARN("Yaw of the datum is ignored!");
    }
    // Try to resolve tf_prefix
    std::string tf_prefix = "";
    std::string tf_prefix_path = "";
    if (nh_priv.searchParam("tf_prefix", tf_prefix_path))
    {
      nh_priv.getParam(tf_prefix_path, tf_prefix);
    }
    
    // Append the tf prefix in a tf2-friendly manner
    GeonavUtilities::appendPrefix(tf_prefix, world_frame_id_);
    GeonavUtilities::appendPrefix(tf_prefix, base_link_frame_id_);
    
    // Convert specified yaw to quaternion
    tf2::Quaternion quat;
    quat.setRPY(0.0, 0.0, datum_yaw);
    // Set datum
    setDatum(datum_lat, datum_lon, 0.0, quat); // alt is 0.0 for now

  } // end of datum config.
  
  // Subscriber - Odometry relative the the GPS frame
  ros::Subscriber odom_sub = nh.subscribe("odometry/nav", 1, &GeonavTransform::navOdomCallback, this);
  
  // Publisher - Odometry relative to the odom frame
  odom_pub_ = nh.advertise<nav_msgs::Odometry>("odometry/odom", 10);
  utm_pub_ = nh.advertise<nav_msgs::Odometry>("odometry/utm", 10);
  
  // Loop
  ros::Rate rate(frequency);
  while (ros::ok())
  {
    ros::spinOnce();
    nav_msgs::Odometry gps_odom;
    rate.sleep();
  } // end of Loop
} // end of ::run()


bool GeonavTransform::setDatum(double lat, double lon, double alt, 
			       tf2::Quaternion q)
{
  double utm_x = 0;
  double utm_y = 0;
  NavsatConversions::LLtoUTM(lat, lon, utm_y, utm_x, utm_zone_);
  
  ROS_INFO_STREAM("Datum (latitude, longitude, altitude) is (" 
		  << std::fixed << lat << ", "
		  << lon << ", " << alt << ")");
  ROS_INFO_STREAM("Datum UTM coordinate is (" 
		  << std::fixed << utm_x << ", " << utm_y << ")");
  
  // Set the tranform utm->odom
  transform_utm2odom_.setOrigin(tf2::Vector3(utm_x, utm_y, alt));
  transform_utm2odom_.setRotation(q);
  transform_utm2odom_inverse_ = transform_utm2odom_.inverse();
  // Convert quaternion to RPY - to double check and diplay
  tf2::Matrix3x3 mat(q);
  double roll, pitch, yaw;
  mat.getRPY(roll, pitch, yaw);
  ROS_INFO_STREAM("Datum orientation roll, pitch, yaw is ("
		  << roll << ", " << pitch << ", " << yaw << ")");
  has_datum_ = true;

  //ROS_INFO_STREAM("Transform utm -> odom is: " << transform_utm2odom_);

  // Send out static UTM transform
  geometry_msgs::TransformStamped utm_transform_stamped;
  utm_transform_stamped.header.stamp = ros::Time::now();
  utm_transform_stamped.header.frame_id = world_frame_id_;
  utm_transform_stamped.child_frame_id = "utm";
  utm_transform_stamped.transform = tf2::toMsg(transform_utm2odom_);
  utm_transform_stamped.transform.translation.z = (zero_altitude_ ? 0.0 : utm_transform_stamped.transform.translation.z);
  utm_broadcaster_.sendTransform(utm_transform_stamped);

} // end setDatum

void GeonavTransform::navOdomCallback(const nav_msgs::OdometryConstPtr& msg)
{
  //world_frame_id_ = msg->header.frame_id;
  //base_link_frame_id_ = msg->child_frame_id;

  nav_frame_id_ = msg->header.frame_id;
  if (nav_frame_id_.empty())
  {
    ROS_WARN_STREAM_ONCE("Odometry message has empty frame_id. "
			 "Will assume navsat device is mounted at "
			 "robot's origin.");
  }
  // Make sure the GPS data is usable - can't use NavSatStatus since we
  // are making due with an Odometry message
  bool good_gps = (!std::isnan(msg->pose.pose.position.x) &&
		   !std::isnan(msg->pose.pose.position.y) &&
		   !std::isnan(msg->pose.pose.position.z));
  if (!good_gps)
  {
    ROS_WARN_STREAM("Bad GPS!  Won't transfrom");
    return;
  }

  double utmX = 0;
  double utmY = 0;
  std::string utm_zone_tmp;
  NavsatConversions::LLtoUTM(msg->pose.pose.position.y, 
			     msg->pose.pose.position.x, 
			     utmY, utmX, utm_zone_tmp);
  ROS_DEBUG_STREAM_THROTTLE(2.0,"Latest GPS (lat, lon, alt): "
			    << msg->pose.pose.position.y << " , "
			    << msg->pose.pose.position.x << " , "
			    << msg->pose.pose.position.z );
  ROS_DEBUG_STREAM_THROTTLE(2.0,"UTM of latest GPS is (X,Y):" 
			    << utmX << " , " << utmY);
  
  transform_utm2nav_.setOrigin(tf2::Vector3(utmX, utmY, 
					  msg->pose.pose.position.z));
  transform_utm2nav_.setRotation(tf2::Quaternion(msg->pose.pose.orientation.x,
						 msg->pose.pose.orientation.y,
						 msg->pose.pose.orientation.z,
						 msg->pose.pose.orientation.w));
  transform_utm2nav_inverse_=transform_utm2nav_.inverse();

  // Transform covariance from nav (sensor) frame to utm
  transform_utm2nav_covariance_.setZero();
  // Populate covariance matrix
  for(size_t row = 0; row < POSE_SIZE; ++row)
  {
    for(size_t col = 0; col < POSE_SIZE; ++col)
    {
      transform_utm2nav_covariance_(row, col) = msg->pose.covariance[row * POSE_SIZE + col];
    }
  }
  // Calculate rotation matrix for position covariance 
  tf2::Matrix3x3 rot(utm_world_trans_inverse_.getRotation());
  Eigen::MatrixXd rot_6d(POSE_SIZE, POSE_SIZE);
  rot_6d.setIdentity();
  for (size_t rInd = 0; rInd < POSITION_SIZE; ++rInd)
  {
    rot_6d(rInd, 0) = rot.getRow(rInd).getX();
    rot_6d(rInd, 1) = rot.getRow(rInd).getY();
    rot_6d(rInd, 2) = rot.getRow(rInd).getZ();
    rot_6d(rInd+POSITION_SIZE, 3) = rot.getRow(rInd).getX();
    rot_6d(rInd+POSITION_SIZE, 4) = rot.getRow(rInd).getY();
    rot_6d(rInd+POSITION_SIZE, 5) = rot.getRow(rInd).getZ();
  }
  // Rotate the covariance
  transform_utm2nav_covariance_ = rot_6d * transform_utm2nav_covariance_.eval() * rot_6d.transpose();

  // Publish Nav in UTM frame
  nav_msgs::Odometry nav_in_utm;
  nav_in_utm.header.frame_id = "utm";
  nav_in_utm.header.stamp = gps_update_time_;
  // Create position information using tranform.
  tf2::toMsg(transform_utm2nav_, nav_in_utm.pose.pose);
  nav_in_utm.pose.pose.position.z = (zero_altitude_ ? 0.0 : nav_in_utm.pose.pose.position.z);
  // Create orientation information directy from incoming orientation
  nav_in_utm.pose.pose.orientation = msg->pose.pose.orientation;
  // Copy in the rotated position covariance information into the message
  for (size_t i = 0; i < POSE_SIZE; i++)
  {
    for (size_t j = 0; j < POSE_SIZE; j++)
    {
      nav_in_utm.pose.covariance[POSITION_SIZE * i + j] = 
	transform_utm2nav_covariance_(i, j);
    }
  }
  // For twist - ignore the rotation for now TOD
  nav_in_utm.twist.twist.linear = msg->twist.twist.linear;
  nav_in_utm.twist.twist.angular = msg->twist.twist.angular;
  nav_in_utm.twist.covariance = msg->twist.covariance;
  // Publish
  utm_pub_.publish(nav_in_utm);


  // Calculate Nav in odom frame
  tf2::Transform transform_odom2nav;
  transform_odom2nav.mult(transform_utm2odom_inverse_,transform_utm2nav_);

  // Set header information stamp because we would like to know the robot's position at that timestamp
  nav_msgs::Odometry nav_in_odom = nav_in_utm;  // only the pose should change!
  nav_in_odom.header.frame_id = world_frame_id_;
  nav_in_odom.header.stamp = gps_update_time_;

  tf2::toMsg(transform_odom2nav, nav_in_odom.pose.pose);
  nav_in_odom.pose.pose.position.z = (zero_altitude_ ? 0.0 : nav_in_odom.pose.pose.position.z);
  odom_pub_.publish(nav_in_odom);

  /*
  // Want the pose of the vehicle origin, not the GPS
  tf2::Transform transformed_utm_robot;
  //getRobotOriginWorldPose(transformed_utm_gps, transformed_utm_robot, nav_in_odom.header.stamp);  TODO
  transformed_utm_robot = transformed_utm_gps;
  
  
  // Rotate the covariance as well
  tf2::Matrix3x3 rot(transform_utm2odom_.getRotation());
  Eigen::MatrixXd rot_6d(POSE_SIZE, POSE_SIZE);
  rot_6d.setIdentity();
  
  for (size_t rInd = 0; rInd < POSITION_SIZE; ++rInd)
  {
    rot_6d(rInd, 0) = rot.getRow(rInd).getX();
    rot_6d(rInd, 1) = rot.getRow(rInd).getY();
    rot_6d(rInd, 2) = rot.getRow(rInd).getZ();
    rot_6d(rInd+POSITION_SIZE, 3) = rot.getRow(rInd).getX();
    rot_6d(rInd+POSITION_SIZE, 4) = rot.getRow(rInd).getY();
    rot_6d(rInd+POSITION_SIZE, 5) = rot.getRow(rInd).getZ();
  }
  
  // Rotate the covariance
  transform_utm2nav_covariance_ = rot_6d * transform_utm2nav_covariance_.eval() * rot_6d.transpose();

  // Now fill out the message. Set the orientation to the identity.
  //tf2::toMsg(transformed_utm_robot, nav_in_odom.pose.pose);

  // Copy the measurement's covariance matrix so that we can rotate it later
  for (size_t i = 0; i < POSE_SIZE; i++)
  {
    for (size_t j = 0; j < POSE_SIZE; j++)
    {
      nav_in_odom.pose.covariance[POSE_SIZE * i + j] = transform_utm2nav_covariance_(i, j);
    }
  }
  
  // Mark this GPS as used
  gps_updated_ = false;
  //new_data = true;
  odom_pub_.publish(nav_in_odom);
  */
}  // navOdomCallback


} // namespace GeonavTransform
