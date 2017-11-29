#ifndef SOCIAL_LAYER_H_
#define SOCIAL_LAYER_H_
#include <ros/ros.h>
#include <costmap_2d/layer.h>
#include <costmap_2d/layered_costmap.h>
#include <object_bridge_msgs/SocialObjectsInFrame.h>
#include <object_bridge_msgs/SocialObject.h>
#include <boost/thread.hpp>

namespace social_navigation_layers
{

class SocialLayer : public costmap_2d::Layer
{
public:
  SocialLayer()
  {
    layered_costmap_ = NULL;
  }

  virtual void onInitialize();
  virtual void updateBounds(double origin_x, double origin_y, double origin_yaw, double* min_x, double* min_y,
                            double* max_x, double* max_y);
  virtual void updateCosts(costmap_2d::Costmap2D& master_grid, int min_i, int min_j, int max_i, int max_j) = 0;

  virtual void updateBoundsFromSocial(double* min_x, double* min_y, double* max_x, double* max_y) = 0;

  bool isDiscretized()
  {
    return false;
  }

protected:
  void socialCallback(const object_bridge_msgs::SocialObjectsInFrame& people);
  ros::Subscriber social_sub_;
  object_bridge_msgs::SocialObjectsInFrame social_list_;
  std::list<object_bridge_msgs::SocialObject> transformed_social_;
  ros::Duration social_keep_time_;
  boost::recursive_mutex lock_;
  tf::TransformListener tf_;
  bool first_time_;
  double last_min_x_, last_min_y_, last_max_x_, last_max_y_;
};
}
;

#endif

