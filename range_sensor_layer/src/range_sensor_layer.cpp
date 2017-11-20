#include <range_sensor_layer/range_sensor_layer.h>
#include <boost/algorithm/string.hpp>
#include <pluginlib/class_list_macros.h>
#include <angles/angles.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>

PLUGINLIB_EXPORT_CLASS(range_sensor_layer::RangeSensorLayer, costmap_2d::Layer)
using costmap_2d::NO_INFORMATION;

const float TRUST_DISTANCE = 0.65;
const float CLOSE_DISTANCE = 0.2;

namespace range_sensor_layer
{

RangeSensorLayer::RangeSensorLayer() {}

void RangeSensorLayer::onInitialize()
{
  ros::NodeHandle nh("~/" + name_);
  current_ = true;
  fusion = false;
  buffered_readings_ = 0;
  last_reading_time_ = ros::Time::now();
  default_value_ = to_cost(0.5);

  matchSize();
  min_x_ = min_y_ = -std::numeric_limits<double>::max();
  max_x_ = max_y_ = std::numeric_limits<double>::max();

  // Default topic names list contains a single topic: /sonar
  // We use the XmlRpcValue constructor that takes a XML string and reading start offset
  const char* xml = "<value><array><data><value>/water_uavcan_master/sonar_filtered</value></data></array></value>";
  int zero_offset = 0;
  std::string topics_ns;
  XmlRpc::XmlRpcValue topic_names(xml, &zero_offset);

  nh.param("ns", topics_ns, std::string());
  nh.param("topics", topic_names, topic_names);

  InputSensorType input_sensor_type = ALL;
  std::string sensor_type_name;
  nh.param("input_sensor_type", sensor_type_name, std::string("ALL"));
  nh.param("clear_on_max_reading", clear_on_max_reading_, true);

  boost::to_upper(sensor_type_name);
  ROS_INFO("%s: %s as input_sensor_type given", name_.c_str(), sensor_type_name.c_str());

  if (sensor_type_name == "VARIABLE")
    input_sensor_type = VARIABLE;
  else if (sensor_type_name == "FIXED")
    input_sensor_type = FIXED;
  else if (sensor_type_name == "ALL")
    input_sensor_type = ALL;
  else
  {
    ROS_ERROR("%s: Invalid input sensor type: %s", name_.c_str(), sensor_type_name.c_str());
  }

  // Validate topic names list: it must be a (normally non-empty) list of strings
  if ((topic_names.valid() == false) || (topic_names.getType() != XmlRpc::XmlRpcValue::TypeArray))
  {
    ROS_ERROR("Invalid topic names list: it must be a non-empty list of strings");
    return;
  }

  if (topic_names.size() < 1)
  {
    // This could be an error, but I keep it as it can be useful for debug
    ROS_WARN("Empty topic names list: range sensor layer will have no effect on costmap");
  }

    // Traverse the topic names list subscribing to all of them with the same callback method
  for (unsigned int i = 0; i < topic_names.size(); i++)
  {
    if (topic_names[i].getType() != XmlRpc::XmlRpcValue::TypeString)
    {
      ROS_WARN("Invalid topic names list: element %d is not a string, so it will be ignored", i);
    }
    else
    {
      std::string topic_name(topics_ns);
      if ((topic_name.size() > 0) && (topic_name.at(topic_name.size() - 1) != '/'))
        topic_name += "/";
      topic_name += static_cast<std::string>(topic_names[i]);

      if (input_sensor_type == VARIABLE)
        processRangeMessageFunc_ = boost::bind(&RangeSensorLayer::processVariableRangeMsg, this, _1);
      else if (input_sensor_type == FIXED)
        processRangeMessageFunc_ = boost::bind(&RangeSensorLayer::processFixedRangeMsg, this, _1);
      else if (input_sensor_type == ALL)
        processRangeMessageFunc_ = boost::bind(&RangeSensorLayer::processRangeMsg, this, _1);
      else
      {
        ROS_ERROR(
            "%s: Invalid input sensor type: %s. Did you make a new type and forgot to choose the subscriber for it?",
            name_.c_str(), sensor_type_name.c_str());
      }

      range_subs_.push_back(nh.subscribe(topic_name, 100, &RangeSensorLayer::bufferIncomingRangeMsg, this));

      ROS_INFO("RangeSensorLayer: subscribed to topic %s", range_subs_.back().getTopic().c_str());
    }
  }

  range_subs_.push_back(nh.subscribe("/scan", 100, &RangeSensorLayer::bufferIncomingScanMsg, this));

  dsrv_ = new dynamic_reconfigure::Server<range_sensor_layer::RangeSensorLayerConfig>(nh);
  dynamic_reconfigure::Server<range_sensor_layer::RangeSensorLayerConfig>::CallbackType cb = boost::bind(
      &RangeSensorLayer::reconfigureCB, this, _1, _2);
  dsrv_->setCallback(cb);
  global_frame_ = layered_costmap_->getGlobalFrameID();

 /*
  message_filters::Subscriber<sensor_msgs::LaserScan> scan_sub(nh, "/scan", 10);
  message_filters::Subscriber<sensor_msgs::Range> sonar_sub(nh, range_subs_.back().getTopic().c_str(), 10);

  message_filters::TimeSynchronizer<sensor_msgs::LaserScan, sensor_msgs::Range> sync(scan_sub, sonar_sub, 60);
  sync.registerCallback(boost::bind(&RangeSensorLayer::syncCB, this, _1, _2));
*/
}


double RangeSensorLayer::gamma(double theta)
{
    if(fabs(theta)>max_angle_)
        return 0.0;
    else
        return 1 - pow(theta/max_angle_, 2);
}

double RangeSensorLayer::delta(double phi)
{
    return 1 - (1+tanh(2*(phi-phi_v_)))/2;
}

void RangeSensorLayer::get_deltas(double angle, double *dx, double *dy)
{
    double ta = tan(angle);
    if(ta==0)
        *dx = 0;
    else
        *dx = resolution_ / ta;

    *dx = copysign(*dx, cos(angle));
    *dy = copysign(resolution_, sin(angle));
}

double RangeSensorLayer::sensor_model(double r, double phi, double theta)
{
    double lbda = delta(phi)*gamma(theta);

    double delta = resolution_;

    if(phi >= 0.0 and phi < r - 2 * delta * r)
        return (1- lbda) * (0.5);
    else if(phi < r - delta * r)
        return lbda* 0.5 * pow((phi - (r - 2*delta*r))/(delta*r), 2)+(1-lbda)*.5;
    else if(phi < r + delta * r){
        double J = (r-phi)/(delta*r);
        return lbda * ((1-(0.5)*pow(J,2)) -0.5) + 0.5;
    }
    else
        return 0.5;
}



void RangeSensorLayer::syncCB(const sensor_msgs::Range& range_message)
{
   scan_message_mutex_.lock();
   sensor_msgs::LaserScan scan_message = scan_msgs_;
   scan_message_mutex_.unlock();

   size_t raduis_start = scan_message.ranges.size()/2 - 50;
   size_t raduis_stop = scan_message.ranges.size()/2 + 50;
   size_t raduis_center = scan_message.ranges.size()/2;
   ROS_INFO("scan_message size %d", scan_message.ranges.size());
   ROS_INFO("scan_message center data %f", scan_message.ranges[raduis_center]);
   ROS_INFO("scan_message start data %f", scan_message.ranges[raduis_start]);
   ROS_INFO("scan_message stop data %f", scan_message.ranges[raduis_stop]);
   ROS_INFO("scan_message time stamp %f", scan_message.header.stamp);
   ROS_INFO("range_message time stamp %f", range_message.header.stamp);
   ROS_INFO("range_message data %f", range_message.range);

   unsigned int count = 0;
   unsigned int inf_count = 0;
   for (size_t i = raduis_start; i < raduis_stop; i++)
   {
      float scan_data = scan_message.ranges[i];
   //   ROS_INFO("scan_data: %f, index %d", scan_data, i);
      if (!std::isfinite(scan_data)) {
         inf_count++;
         continue;
      }

      if (scan_data < range_message.range + TRUST_DISTANCE) {
         ROS_INFO("scan data %f", scan_data);
     //  count++;
         fusion = true;
         return;
      }

   }
/*
      ROS_INFO("count %d", inf_count);
   if (inf_count > THRESHOLD_MIN && inf_count < THRESHOLD_MAX) {
      ROS_INFO("inf_count %d", inf_count);
       fusion = true;
       return;
   }
   else
*/
       fusion = false;
}

void RangeSensorLayer::reconfigureCB(range_sensor_layer::RangeSensorLayerConfig &config, uint32_t level)
{
  phi_v_ = config.phi;
  max_angle_ = config.max_angle;
  no_readings_timeout_ = config.no_readings_timeout;
  mark_threshold_ = config.mark_threshold;
  clear_on_max_reading_ = config.clear_on_max_reading;

  if(enabled_ != config.enabled)
  {
    enabled_ = config.enabled;
    current_ = false;
  }
}

void RangeSensorLayer::bufferIncomingScanMsg(const sensor_msgs::LaserScanConstPtr& scan_message)
{
    boost::mutex::scoped_lock lock(scan_message_mutex_);
    scan_msgs_ = *scan_message;
}

void RangeSensorLayer::bufferIncomingRangeMsg(const sensor_msgs::RangeConstPtr& range_message)
{
 // ROS_INFO("range coming");
  boost::mutex::scoped_lock lock(range_message_mutex_);
  range_msgs_buffer_.push_back(*range_message);
}

void RangeSensorLayer::updateCostmap()
{
  std::list<sensor_msgs::Range> range_msgs_buffer_copy;

  range_message_mutex_.lock();
  range_msgs_buffer_copy = std::list<sensor_msgs::Range>(range_msgs_buffer_);
  range_msgs_buffer_.clear();
  range_message_mutex_.unlock();

  for (std::list<sensor_msgs::Range>::iterator range_msgs_it = range_msgs_buffer_copy.begin();
      range_msgs_it != range_msgs_buffer_copy.end(); range_msgs_it++)
  {
    processRangeMessageFunc_(*range_msgs_it);
  }
}

void RangeSensorLayer::processRangeMsg(sensor_msgs::Range& range_message)
{
  if (range_message.min_range == range_message.max_range)
    processFixedRangeMsg(range_message);
  else
    processVariableRangeMsg(range_message);
}

void RangeSensorLayer::processFixedRangeMsg(sensor_msgs::Range& range_message)
{
  if (!isinf(range_message.range))
  {
    ROS_ERROR_THROTTLE(1.0,
        "Fixed distance ranger (min_range == max_range) in frame %s sent invalid value. Only -Inf (== object detected) and Inf (== no object detected) are valid.",
        range_message.header.frame_id.c_str());
    return;
  }

  bool clear_sensor_cone = false;

  if (range_message.range > 0) //+inf
  {
    if (!clear_on_max_reading_)
      return; //no clearing at all

    clear_sensor_cone = true;
  }

  range_message.range = range_message.min_range;

  updateCostmap(range_message, clear_sensor_cone);
}

void RangeSensorLayer::processVariableRangeMsg(sensor_msgs::Range& range_message)
{
  ROS_INFO("RANGE MAX, %f", range_message.max_range);
  ROS_INFO("range min, %f", range_message.min_range);
  ROS_INFO("RANGE DATA,%f", range_message.range);
  if (range_message.range <= range_message.min_range)
    return;

  bool clear_sensor_cone = false;
  syncCB(range_message);
  if ((range_message.range >= range_message.max_range) ||
       fusion)
    clear_sensor_cone = true;

    ROS_INFO("CLEAR SENSOR CONE %d", clear_sensor_cone);
    updateCostmap(range_message, clear_sensor_cone);
}

void RangeSensorLayer::updateCostmap(sensor_msgs::Range& range_message, bool clear_sensor_cone)
{
  max_angle_ = range_message.field_of_view/2;

  geometry_msgs::PointStamped in, out;
  in.header.stamp = range_message.header.stamp;
  in.header.frame_id = range_message.header.frame_id;

  if(!tf_->waitForTransform(global_frame_, in.header.frame_id,
        in.header.stamp, ros::Duration(0.1)) ) {
     ROS_ERROR_THROTTLE(1.0, "Range sensor layer can't transform from %s to %s at %f",
        global_frame_.c_str(), in.header.frame_id.c_str(),
        in.header.stamp.toSec());
     return;
  }

  tf_->transformPoint (global_frame_, in, out);

  double ox = out.point.x, oy = out.point.y;

  in.point.x = range_message.range;

  tf_->transformPoint(global_frame_, in, out);

  double tx = out.point.x, ty = out.point.y;

  // calculate target props
  double dx = tx-ox, dy = ty-oy,
        theta = atan2(dy,dx), d = sqrt(dx*dx+dy*dy);

  // Integer Bounds of Update
  int bx0, by0, bx1, by1;

  // Bounds includes the origin
  worldToMapNoBounds(ox, oy, bx0, by0);
  bx1 = bx0;
  by1 = by0;
  touch(ox, oy, &min_x_, &min_y_, &max_x_, &max_y_);

  // Update Map with Target Point
  unsigned int aa, ab;
  if(worldToMap(tx, ty, aa, ab)){
    setCost(aa, ab, 233);
    touch(tx, ty, &min_x_, &min_y_, &max_x_, &max_y_);
  }

  double mx, my;
  int a, b;
  costmap_2d::Costmap2D* costmap = layered_costmap_->getCostmap();
  double res = costmap->getResolution();
  double radius = d * tanh(max_angle_);

  if (range_message.range >= CLOSE_DISTANCE && range_message.range < range_message.max_range)
  {
    for (double r = -1.0 * radius; r < radius; r += res)
    {
      mx = tx - r * sin(theta);
      my = ty + r * cos(theta);
      worldToMapNoBounds(mx, my, a, b);
      setCost(a, b, 233);
    }

  }

  // Update left side of sonar cone
  mx = tx - radius * sin(theta);
  my = ty + radius * cos(theta);
  worldToMapNoBounds(mx, my, a, b);
  bx0 = std::min(bx0, a);
  bx1 = std::max(bx1, a);
  by0 = std::min(by0, b);
  by1 = std::max(by1, b);
  touch(mx, my, &min_x_, &min_y_, &max_x_, &max_y_);

  // Update left side of sonar cone
  mx = tx + radius * sin(theta);
  my = ty - radius * cos(theta);
  worldToMapNoBounds(mx, my, a, b);
  bx0 = std::min(bx0, a);
  bx1 = std::max(bx1, a);
  by0 = std::min(by0, b);
  by1 = std::max(by1, b);
  touch(mx, my, &min_x_, &min_y_, &max_x_, &max_y_);


  // Limit Bounds to Grid
  bx0 = std::max(0, bx0);
  by0 = std::max(0, by0);
  bx1 = std::min((int)size_x_, bx1);
  by1 = std::min((int)size_y_, by1);

  for(unsigned int x=bx0; x<=(unsigned int)bx1; x++){
    for(unsigned int y=by0; y<=(unsigned int)by1; y++){
      double wx, wy;
      mapToWorld(x,y,wx,wy);
      update_cell(ox, oy, theta, range_message.range, wx, wy, clear_sensor_cone);
    }
  }

  buffered_readings_++;
  last_reading_time_ = ros::Time::now();
}

void RangeSensorLayer::update_cell(double ox, double oy, double ot, double r, double nx, double ny, bool clear)
{
  unsigned int x, y;
  if(worldToMap(nx, ny, x, y)){
    double dx = nx-ox, dy = ny-oy;
    double theta = atan2(dy, dx) - ot;
    theta = angles::normalize_angle(theta);
    double phi = sqrt(dx*dx+dy*dy);
    double sensor = 0.0;

    if (clear && fabs(theta) > max_angle_)
      return;

    if(!clear)
        sensor = sensor_model(r,phi,theta);
    double prior = to_prob(getCost(x,y));
    double prob_occ = sensor * prior;
    double prob_not = (1 - sensor) * (1 - prior);
    double new_prob = prob_occ/(prob_occ+prob_not);

    //ROS_INFO("%f %f | %f %f = %f", dx, dy, theta, phi, sensor);
    //ROS_INFO("%f | %f %f | %f", prior, prob_occ, prob_not, new_prob);
      unsigned char c = to_cost(new_prob);
      setCost(x,y,c);
  }
}

void RangeSensorLayer::updateBounds(double robot_x, double robot_y, double robot_yaw, double* min_x,
                                           double* min_y, double* max_x, double* max_y)
{
  if (layered_costmap_->isRolling())
    updateOrigin(robot_x - getSizeInMetersX() / 2, robot_y - getSizeInMetersY() / 2);

  updateCostmap();

  *min_x = std::min(*min_x, min_x_);
  *min_y = std::min(*min_y, min_y_);
  *max_x = std::max(*max_x, max_x_);
  *max_y = std::max(*max_y, max_y_);

  min_x_ = min_y_ = std::numeric_limits<double>::max();
  max_x_ = max_y_ = std::numeric_limits<double>::min();

  if (!enabled_)
  {
    current_ = true;
    return;
  }

  if (buffered_readings_ == 0)
  {
    if (no_readings_timeout_ > 0.0 &&
        (ros::Time::now() - last_reading_time_).toSec() > no_readings_timeout_)
    {
      ROS_WARN_THROTTLE(2.0, "No range readings received for %.2f seconds, " \
                             "while expected at least every %.2f seconds.",
               (ros::Time::now() - last_reading_time_).toSec(), no_readings_timeout_);
      current_ = false;
    }
  }

}

void RangeSensorLayer::updateCosts(costmap_2d::Costmap2D& master_grid, int min_i, int min_j, int max_i,
                                          int max_j)
{
  if (!enabled_)
    return;

  unsigned char* master_array = master_grid.getCharMap();
  unsigned int span = master_grid.getSizeInCellsX();
  unsigned char clear = to_cost(clear_threshold_), mark = to_cost(mark_threshold_);

  for (int j = min_j; j < max_j; j++)
  {
    unsigned int it = j * span + min_i;
    for (int i = min_i; i < max_i; i++)
    {
      unsigned char prob = costmap_[it];
      unsigned char current;
      if(prob==costmap_2d::NO_INFORMATION){
        it++;
        continue;
      }
      else if(prob>mark)
        current = costmap_2d::LETHAL_OBSTACLE;
      else if(prob<clear)
        current = costmap_2d::FREE_SPACE;
      else{
        it++;
        continue;
      }

      unsigned char old_cost = master_array[it];

      if (old_cost == NO_INFORMATION || old_cost < current)
        master_array[it] = current;
      it++;
    }
  }

  buffered_readings_ = 0;
  current_ = true;
}

void RangeSensorLayer::reset()
{
  ROS_DEBUG("Reseting range sensor layer...");
  deactivate();
  resetMaps();
  current_ = true;
  activate();
}

void RangeSensorLayer::deactivate()
{
  range_msgs_buffer_.clear();
}

void RangeSensorLayer::activate()
{
  range_msgs_buffer_.clear();
}

} // end namespace
