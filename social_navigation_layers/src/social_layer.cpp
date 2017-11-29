#include <social_navigation_layers/social_layer.h>
#include <math.h>
#include <angles/angles.h>
#include <pluginlib/class_list_macros.h>

using costmap_2d::NO_INFORMATION;
using costmap_2d::LETHAL_OBSTACLE;
using costmap_2d::FREE_SPACE;

namespace social_navigation_layers
{
void SocialLayer::onInitialize()
{
  ros::NodeHandle nh("~/" + name_), g_nh;
  current_ = true;
  first_time_ = true;
  social_sub_ = nh.subscribe("/ca_policy/social_object", 1, &SocialLayer::socialCallback, this);
}

void SocialLayer::socialCallback(const object_bridge_msgs::SocialObjectsInFrame& people)
{
  boost::recursive_mutex::scoped_lock lock(lock_);
  social_list_ = people;
}

void SocialLayer::updateBounds(double origin_x, double origin_y, double origin_z, double* min_x, double* min_y,
                               double* max_x, double* max_y)
{
  boost::recursive_mutex::scoped_lock lock(lock_);

  std::string global_frame = layered_costmap_->getGlobalFrameID();
  transformed_social_.clear();

  for (unsigned int i = 0; i < social_list_.objects.size(); i++)
  {
    object_bridge_msgs::SocialObject& person = social_list_.objects[i];
    object_bridge_msgs::SocialObject tpt;
    geometry_msgs::PointStamped pt, opt;

    try
    {
      pt.point.x = person.position.x;
      pt.point.y = person.position.y;
      pt.point.z = person.position.z;
      pt.header.frame_id = social_list_.header.frame_id;
      tf_.transformPoint(global_frame, pt, opt);
      tpt.position.x = opt.point.x;
      tpt.position.y = opt.point.y;
      tpt.position.z = opt.point.z;

      pt.point.x += person.velocity.x;
      pt.point.y += person.velocity.y;
      pt.point.z += person.velocity.z;
      tf_.transformPoint(global_frame, pt, opt);

      tpt.velocity.x = opt.point.x;
      tpt.velocity.y = opt.point.y;
      tpt.velocity.z = opt.point.z;

      transformed_social_.push_back(tpt);

    }
    catch (tf::LookupException& ex)
    {
      ROS_ERROR("No Transform available Error: %s\n", ex.what());
      continue;
    }
    catch (tf::ConnectivityException& ex)
    {
      ROS_ERROR("Connectivity Error: %s\n", ex.what());
      continue;
    }
    catch (tf::ExtrapolationException& ex)
    {
      ROS_ERROR("Extrapolation Error: %s\n", ex.what());
      continue;
    }
  }
  updateBoundsFromSocial(min_x, min_y, max_x, max_y);
  if (first_time_)
  {
    last_min_x_ = *min_x;
    last_min_y_ = *min_y;
    last_max_x_ = *max_x;
    last_max_y_ = *max_y;
    first_time_ = false;
  }
  else
  {
    double a = *min_x, b = *min_y, c = *max_x, d = *max_y;
    *min_x = std::min(last_min_x_, *min_x);
    *min_y = std::min(last_min_y_, *min_y);
    *max_x = std::max(last_max_x_, *max_x);
    *max_y = std::max(last_max_y_, *max_y);
    last_min_x_ = a;
    last_min_y_ = b;
    last_max_x_ = c;
    last_max_y_ = d;

  }

}
}
;
