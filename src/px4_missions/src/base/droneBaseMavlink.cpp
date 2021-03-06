#include "droneBaseMavlink.hpp"

DroneMavlink::DroneMavlink() : Node("DroneMavlink")
{
	this->declare_parameter("vehicleName", "");
	this->declare_parameter("takeOffHeight", 10.0);
	this->declare_parameter("xyMaxVelocity", 10.0);
	
	param.vehicleName = this->get_parameter("vehicleName").as_string();
	param.takeOffHeight = this->get_parameter("takeOffHeight").as_double();
	param.xyMaxVelocity = this->get_parameter("xyMaxVelocity").as_double();

	_pos_setp_pub = this->create_publisher<geometry_msgs::msg::PoseStamped>(param.vehicleName + "/mavros/setpoint_position/local", 1);
	_vel_setp_pub = this->create_publisher<geometry_msgs::msg::TwistStamped>(param.vehicleName + "/mavros/setpoint_velocity/cmd_vel", 1);
	_geo_setp_pub = this->create_publisher<geographic_msgs::msg::GeoPoseStamped>(param.vehicleName + "/mavros/setpoint_position/global", 1);

	_state_sub = this->create_subscription<mavros_msgs::msg::State>(
										param.vehicleName + "/mavros/state", 
										1, 
										[this](mavros_msgs::msg::State::ConstSharedPtr msg) {
											currentState = *msg;
										});

	_alt_sub = this->create_subscription<mavros_msgs::msg::Altitude>(
										param.vehicleName + "/mavros/altitude", 
										rclcpp::SensorDataQoS(), 
										[this](mavros_msgs::msg::Altitude::ConstSharedPtr msg) {
											altitude = msg->amsl;
										});


	_gps_sub = this->create_subscription<sensor_msgs::msg::NavSatFix>(
										param.vehicleName + "/mavros/global_position/raw/fix", 
										rclcpp::SensorDataQoS(), 
										[this](sensor_msgs::msg::NavSatFix::ConstSharedPtr msg) {
											gpsPos = *msg;
										});


	_loc_pose_sub = this->create_subscription<geometry_msgs::msg::PoseStamped>(
										param.vehicleName + "/mavros/local_position/pose", 
										rclcpp::SensorDataQoS(),
										[this](geometry_msgs::msg::PoseStamped::ConstSharedPtr msg) {
											locPos = *msg;
										});



	_cmd_cli = this->create_client<mavros_msgs::srv::CommandBool>(param.vehicleName + "/mavros/cmd/arming");
	_mode_cli = this->create_client<mavros_msgs::srv::SetMode>(param.vehicleName + "/mavros/set_mode");
	_param_cli = this->create_client<mavros_msgs::srv::ParamSetV2>(param.vehicleName + "/mavros/param/set");
	_param_req_cli = this->create_client<mavros_msgs::srv::ParamPull>(param.vehicleName + "/mavros/param/pull");

}

void DroneMavlink::pullParam()
{
	// check if services exist
	while (!_param_req_cli->wait_for_service(1s))
	{
		if (!rclcpp::ok())
		{
			RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
			exit(1);
		}
		RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
	}
  	auto request = std::make_shared<mavros_msgs::srv::ParamPull::Request>();
	request->force_pull = false;
	
	// wait for result
    using ServiceResponseFuture = rclcpp::Client<mavros_msgs::srv::ParamPull>::SharedFuture;
    auto response_received_callback = [this](ServiceResponseFuture future)
	{
		auto result = future.get();
		paramPullOk = result->success;
		if(!paramPullOk)
			RCLCPP_ERROR(this->get_logger(), "PARAMETERS WERE NOT PULLED");
    };
    auto future_result = _param_req_cli->async_send_request(request, response_received_callback);
}


void DroneMavlink::preFlightCheck()
{
	changeParam("MIS_TAKEOFF_ALT", 3, 0, param.takeOffHeight);
	RCLCPP_INFO(this->get_logger(), "TakeOff height set to %f", param.takeOffHeight);

	changeParam("COM_RCL_EXCEPT", 2, 4, 0);
	RCLCPP_INFO(this->get_logger(), "RC loss exceptions is set");

	changeParam("COM_OBL_ACT", 2, 1, 0);
	RCLCPP_INFO(this->get_logger(), "Failsafe action after Offboard mode lost");

	// max 20 m/s
	changeParam("MPC_XY_VEL_MAX", 3, 0, param.xyMaxVelocity);
	RCLCPP_INFO(this->get_logger(), "Horizontal speed set to %f", param.xyMaxVelocity);
}


void DroneMavlink::changeParam(std::string name, int type, int intVal, float floatVal)
{
	_paramsToChange++;
	// check if services exist
	while (!_param_cli->wait_for_service(1s))
	{
		if (!rclcpp::ok())
		{
			RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
			exit(1);
		}
		RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
	}

  	auto request = std::make_shared<mavros_msgs::srv::ParamSetV2::Request>();

	request->param_id = name;
	request->value.type = type;
	request->value.integer_value = intVal;
	request->value.double_value = floatVal;

	// wait for result
    using ServiceResponseFuture = rclcpp::Client<mavros_msgs::srv::ParamSetV2>::SharedFuture;
    auto response_received_callback = [this](ServiceResponseFuture future)
	{
		auto result = future.get();
		if(!result->success)
			RCLCPP_ERROR(this->get_logger(), "ERROR SETTING PARAMETER");

		if(result->success)
			_changedParams++;

		// check, if all parameters were changed and if we have gps fix
		if(gpsPos.status.status >= 0 && _changedParams == _paramsToChange)
			preFlightCheckOK = true;
		else
			preFlightCheckOK = false;
    };
    auto future_result = _param_cli->async_send_request(request, response_received_callback);
}


void DroneMavlink::setFlightMode(FlightMode mode)
{
	// check if services exist
	while (!_mode_cli->wait_for_service(1s))
	{
		if (!rclcpp::ok())
		{
			RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
			exit(1);
		}
		RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
	}
			
	auto request = std::make_shared<mavros_msgs::srv::SetMode::Request>();

	switch (mode)
	{
		case FlightMode::mOffboard:
			request->base_mode = 0;
			request->custom_mode = "OFFBOARD";
			_flightMode = "OFFBOARD";
			break;

		case FlightMode::mTakeOff:
			request->base_mode = 0;
			request->custom_mode = "AUTO.TAKEOFF";
			_flightMode = "TAKEOFF";
			break;
			
		case FlightMode::mLand:
			request->base_mode = 0;
			request->custom_mode = "AUTO.LAND";
			_flightMode = "LAND";
			break;
			
		case FlightMode::mReturnToLaunch:
			request->base_mode = 0;
			request->custom_mode = "AUTO.RTL";
			_flightMode = "RTL";
			break;

		case FlightMode::mHold:
			request->base_mode = 0;
			request->custom_mode = "AUTO.LOITER";
			_flightMode = "HOLD";
			break;

		// NOT YET WORKING
		case FlightMode::mMission:
			request->base_mode = 0;
			request->custom_mode = "AUTO.MISSION";
			_flightMode = "MISSION";
			break;
			
		default:
			RCLCPP_INFO(this->get_logger(), "No flight mode set");
	}

	// wait for result
    using ServiceResponseFuture = rclcpp::Client<mavros_msgs::srv::SetMode>::SharedFuture;
    auto response_received_callback = [this](ServiceResponseFuture future)
	{
		auto result = future.get();
		if(result->mode_sent)
			RCLCPP_INFO(this->get_logger(), "%s flight mode set", _flightMode.c_str());
		else
			RCLCPP_ERROR(this->get_logger(), "FLIGHT MODE CHANGE ERROR");
    };
    auto future_result = _mode_cli->async_send_request(request, response_received_callback);

}


void DroneMavlink::arm()
{
	// check if services exist
	while (!_cmd_cli->wait_for_service(1s))
	{
		if (!rclcpp::ok())
		{
			RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
			exit(1);
		}
		RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
	}

  	auto request = std::make_shared<mavros_msgs::srv::CommandBool::Request>();
	request->value = true;

	// wait for result
    using ServiceResponseFuture = rclcpp::Client<mavros_msgs::srv::CommandBool>::SharedFuture;
    auto response_received_callback = [this](ServiceResponseFuture future)
	{
		auto result = future.get();
		if(result->success)
			RCLCPP_INFO(this->get_logger(), "Arm command send");
		else
			RCLCPP_ERROR(this->get_logger(), "ARMING ERROR");
    };
    auto future_result = _cmd_cli->async_send_request(request, response_received_callback);
}


void DroneMavlink::disarm()
{
	// check if services exist
	while (!_cmd_cli->wait_for_service(1s))
	{
		if (!rclcpp::ok())
		{
			RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
			exit(1);
		}
		RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
	}

  	auto request = std::make_shared<mavros_msgs::srv::CommandBool::Request>();
	request->value = false;

	// wait for result
    using ServiceResponseFuture = rclcpp::Client<mavros_msgs::srv::CommandBool>::SharedFuture;
    auto response_received_callback = [this](ServiceResponseFuture future)
	{
		auto result = future.get();
		if(result->success)
			RCLCPP_INFO(this->get_logger(), "Disarm command send");
		else
			RCLCPP_ERROR(this->get_logger(), "DISARMING ERROR");
    };
    auto future_result = _cmd_cli->async_send_request(request, response_received_callback);
}


void DroneMavlink::publish_traj_setp_position(float x, float y, float z, float yaw)
{
	geometry_msgs::msg::PoseStamped msg;
    msg.pose.position.x = x;
    msg.pose.position.y = y;
    msg.pose.position.z = z;
	tf2::Quaternion q;
	q.setRPY(0, 0, yaw);
	msg.pose.orientation = tf2::toMsg(q);
	_pos_setp_pub->publish(msg);
}


void DroneMavlink::publish_traj_setp_speed(float vx, float vy, float vz, float yawspeed)
{
	geometry_msgs::msg::TwistStamped msg;
    msg.twist.linear.x = vx;
    msg.twist.linear.y = vy;
    msg.twist.linear.z = vz;
	msg.twist.angular.z = yawspeed;
	_vel_setp_pub->publish(msg);
}

void DroneMavlink::publish_traj_setp_geo(float lat, float lon, float alt, bool heading)
{
	geographic_msgs::msg::GeoPoseStamped msg;
    msg.pose.position.latitude = lat;
    msg.pose.position.longitude = lon;
    msg.pose.position.altitude = alt;

	tf2::Quaternion q;

	if(heading)
	{
		if(isRotEnable())
			_lastAzimut = azimutToSetp();
		q.setRPY(0, 0, _lastAzimut);
	}
	else
		q.setRPY(0, 0, 0);

	msg.pose.orientation = tf2::toMsg(q);

	_geo_setp_pub->publish(msg);

	lastGlobalSetpoint.lat = lat;
	lastGlobalSetpoint.lon = lon;
	lastGlobalSetpoint.alt = alt;
}


bool DroneMavlink::isGlSetpReached()
{   
	if(distToSetp() <= param.setpReachedDist)
		return true;
	else
		return false;
}

bool DroneMavlink::isRotEnable()
{   
	if(distToSetp() >= param.disableRotationDist)
		return true;
	else
		return false;
}

float DroneMavlink::distToSetp()
{
	float latSetpRad = 3.1415 * lastGlobalSetpoint.lat / 180;
	float latActRad = 3.1415 * gpsPos.latitude / 180;

	float latDiff = 3.1415 * (lastGlobalSetpoint.lat - gpsPos.latitude) / 180;
	float lonDiff = 3.1415 * (lastGlobalSetpoint.lon - gpsPos.longitude) / 180;

    float  a = sin(latDiff/2) * sin(latDiff/2) + cos(latSetpRad) * cos(latActRad) * sin(lonDiff/2) * sin(lonDiff/2);
    float  c = 2 * atan2(sqrt(a), sqrt(1-a));
    float  distance = 6372797.56085 * c;

	distance += abs(lastGlobalSetpoint.alt - altitude);
	return distance;
}

float DroneMavlink::azimutToSetp()
{
	float latSetpRad = 3.1415 * lastGlobalSetpoint.lat / 180;
	float latActRad = 3.1415 * gpsPos.latitude / 180;

	float lonDiff = 3.1415 * (lastGlobalSetpoint.lon - gpsPos.longitude) / 180;

	float y = sin(lonDiff) * cos(latSetpRad);
	float x = cos(latActRad)*sin(latSetpRad) - sin(latActRad)*cos(latSetpRad)*cos(lonDiff);
	float th = atan2(x,y);
	return th;
}