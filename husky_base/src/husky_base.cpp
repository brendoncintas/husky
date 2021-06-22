#include "husky_base/husky_base.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "rclcpp/rclcpp.hpp"

namespace
{
  const uint8_t LEFT = 0, RIGHT = 1;
}

namespace husky_base
{
  static const std::string HW_NAME = "HuskyBase";

  /**
  * Get current encoder travel offsets from MCU and bias future encoder readings against them
  */
  void HuskyBase::resetTravelOffset()
  {
    horizon_legacy::Channel<clearpath::DataEncoders>::Ptr enc = horizon_legacy::Channel<clearpath::DataEncoders>::requestData(
      polling_timeout_);
    if (enc)
    {
      for (auto i = 0u; i < hw_states_position_offset_.size(); i++)
      {
        hw_states_position_offset_[i] = linearToAngular(enc->getTravel(i % 2));
      }
    }
    else
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(HW_NAME), "Could not get encoder data to calibrate travel offset");
    }
  }

  /**
  * Husky reports travel in metres, need radians for ros_control RobotHW
  */
  double HuskyBase::linearToAngular(const double &travel) const
  {
    return (travel / wheel_diameter_ * 2.0f);
  }

  /**
  * RobotHW provides velocity command in rad/s, Husky needs m/s,
  */
  double HuskyBase::angularToLinear(const double &angle) const
  {
    return (angle * wheel_diameter_ / 2.0f);
  }

  void HuskyBase::writeCommandsToHardware()
  {
    double diff_speed_left = angularToLinear(hw_commands_[LEFT]);
    double diff_speed_right = angularToLinear(hw_commands_[RIGHT]);

    limitDifferentialSpeed(diff_speed_left, diff_speed_right);

    horizon_legacy::controlSpeed(diff_speed_left, diff_speed_right, max_accel_, max_accel_);
  }

  void HuskyBase::limitDifferentialSpeed(double &diff_speed_left, double &diff_speed_right)
  {
    double large_speed = std::max(std::abs(diff_speed_left), std::abs(diff_speed_right));

    if (large_speed > max_speed_)
    {
      diff_speed_left *= max_speed_ / large_speed;
      diff_speed_right *= max_speed_ / large_speed;
    }
  }


  /**
  * Pull latest speed and travel measurements from MCU, and store in joint structure for ros_control
  */
  void HuskyBase::updateJointsFromHardware()
  {

    horizon_legacy::Channel<clearpath::DataEncoders>::Ptr enc =
      horizon_legacy::Channel<clearpath::DataEncoders>::requestData(polling_timeout_);
    if (enc)
    {
      RCLCPP_WARN(
        rclcpp::get_logger(HW_NAME),
        "Received linear distance information (L: %f, R: %f)",
        enc->getTravel(LEFT), enc->getTravel(RIGHT));
      for (auto i = 0; i < hw_states_position_.size(); i++)
      {
        double delta = linearToAngular(enc->getTravel(i % 2)) - hw_states_position_[i] - hw_states_position_offset_[i];

        // detect suspiciously large readings, possibly from encoder rollover
        if (std::abs(delta) < 1.0f)
        {
          hw_states_position_[i] += delta;
        }
        else
        {
          // suspicious! drop this measurement and update the offset for subsequent readings
          hw_states_position_offset_[i] += delta;
          RCLCPP_INFO(
            rclcpp::get_logger(HW_NAME),"Dropping overflow measurement from encoder");
        }
      }
    }

    horizon_legacy::Channel<clearpath::DataDifferentialSpeed>::Ptr speed =
      horizon_legacy::Channel<clearpath::DataDifferentialSpeed>::requestData(polling_timeout_);
    if (speed)
    {
      RCLCPP_WARN(
        rclcpp::get_logger(HW_NAME),
        "Received linear speed information (L: %f, R: %f)",
        speed->getLeftSpeed(), speed->getRightSpeed());

      for (auto i = 0; i < hw_states_velocity_.size(); i++)
      {
        if (i % 2 == LEFT)
        {
          hw_states_velocity_[i] = linearToAngular(speed->getLeftSpeed());
        }
        else
        { // assume RIGHT
          hw_states_velocity_[i] = linearToAngular(speed->getRightSpeed());
        }
      }
    }
  }


hardware_interface::return_type HuskyBase::configure(
  const hardware_interface::HardwareInfo & info)
{
  if (configure_default(info) != hardware_interface::return_type::OK)
  {
    return hardware_interface::return_type::ERROR;
  }

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Name: %s", info_.name.c_str());

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Number of Joints %u", info_.joints.size());

  hw_start_sec_ = std::stod(info_.hardware_parameters["hw_start_duration_sec"]);
  hw_stop_sec_ = std::stod(info_.hardware_parameters["hw_stop_duration_sec"]);
  hw_states_position_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  hw_states_position_offset_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  hw_states_velocity_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  hw_commands_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());

  wheel_diameter_ = std::stod(info_.hardware_parameters["wheel_diameter"]);
  max_accel_ = std::stod(info_.hardware_parameters["max_accel"]);
  max_speed_ = std::stod(info_.hardware_parameters["max_speed"]);
  polling_timeout_ = std::stod(info_.hardware_parameters["polling_timeout"]);

  serial_port_ = info_.hardware_parameters["serial_port"];

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Port: %s", serial_port_.c_str());
  horizon_legacy::connect(serial_port_);
  horizon_legacy::configureLimits(max_speed_, max_accel_);
  resetTravelOffset();

  for (const hardware_interface::ComponentInfo & joint : info_.joints)
  {
    // HuskyBase has exactly two states and one command interface on each joint
    if (joint.command_interfaces.size() != 1)
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(HW_NAME),
        "Joint '%s' has %d command interfaces found. 1 expected.", joint.name.c_str(),
        joint.command_interfaces.size());
      return hardware_interface::return_type::ERROR;
    }

    if (joint.command_interfaces[0].name != hardware_interface::HW_IF_VELOCITY)
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(HW_NAME),
        "Joint '%s' have %s command interfaces found. '%s' expected.", joint.name.c_str(),
        joint.command_interfaces[0].name.c_str(), hardware_interface::HW_IF_VELOCITY);
      return hardware_interface::return_type::ERROR;
    }

    if (joint.state_interfaces.size() != 2)
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(HW_NAME),
        "Joint '%s' has %d state interface. 2 expected.", joint.name.c_str(),
        joint.state_interfaces.size());
      return hardware_interface::return_type::ERROR;
    }

    if (joint.state_interfaces[0].name != hardware_interface::HW_IF_POSITION)
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(HW_NAME),
        "Joint '%s' have '%s' as first state interface. '%s' and '%s' expected.",
        joint.name.c_str(), joint.state_interfaces[0].name.c_str(),
        hardware_interface::HW_IF_POSITION);
      return hardware_interface::return_type::ERROR;
    }

    if (joint.state_interfaces[1].name != hardware_interface::HW_IF_VELOCITY)
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(HW_NAME),
        "Joint '%s' have '%s' as second state interface. '%s' expected.", joint.name.c_str(),
        joint.state_interfaces[1].name.c_str(), hardware_interface::HW_IF_VELOCITY);
      return hardware_interface::return_type::ERROR;
    }
  }

  status_ = hardware_interface::status::CONFIGURED;
  return hardware_interface::return_type::OK;
}

std::vector<hardware_interface::StateInterface> HuskyBase::export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface> state_interfaces;
  for (auto i = 0u; i < info_.joints.size(); i++)
  {
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      info_.joints[i].name, hardware_interface::HW_IF_POSITION, &hw_states_position_[i]));
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &hw_states_velocity_[i]));
  }

  return state_interfaces;
}

std::vector<hardware_interface::CommandInterface> HuskyBase::export_command_interfaces()
{
  std::vector<hardware_interface::CommandInterface> command_interfaces;

  for (auto i = 0u; i < info_.joints.size(); i++)
  {
    command_interfaces.emplace_back(hardware_interface::CommandInterface(
      info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &hw_commands_[i]));
  }

  return command_interfaces;
}

hardware_interface::return_type HuskyBase::start()
{
  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Starting ...please wait...");

  for (auto i = 0; i <= hw_start_sec_; i++)
  {
    rclcpp::sleep_for(std::chrono::seconds(1));
    RCLCPP_INFO(
      rclcpp::get_logger(HW_NAME), "%.1f seconds left...", hw_start_sec_ - i);
  }

  // set some default values
  for (auto i = 0u; i < hw_states_position_.size(); i++)
  {
    if (std::isnan(hw_states_position_[i]))
    {
      hw_states_position_[i] = 0;
      hw_states_position_offset_[i] = 0;
      hw_states_velocity_[i] = 0;
      hw_commands_[i] = 0;
    }
  }

  status_ = hardware_interface::status::STARTED;

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "System Successfully started!");

  return hardware_interface::return_type::OK;
}

hardware_interface::return_type HuskyBase::stop()
{
  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Stopping ...please wait...");

  for (auto i = 0u; i <= hw_stop_sec_; i++)
  {
    rclcpp::sleep_for(std::chrono::seconds(1));
    RCLCPP_INFO(
      rclcpp::get_logger(HW_NAME), "%.1f seconds left...", hw_stop_sec_ - i);
  }

  status_ = hardware_interface::status::STOPPED;

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "System successfully stopped!");

  return hardware_interface::return_type::OK;
}

hardware_interface::return_type HuskyBase::read()
{
  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Reading from hardware");

  updateJointsFromHardware();

  for (auto i = 0u; i < hw_states_velocity_.size(); i++)
  {
    RCLCPP_INFO(
      rclcpp::get_logger(HW_NAME),
      "Got position state %.5f and velocity state %.5f for '%s'!",
      hw_states_velocity_[i], hw_states_position_[i], info_.joints[i].name.c_str());
  }

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Joints successfully read!");

  return hardware_interface::return_type::OK;
}

hardware_interface::return_type HuskyBase::write()
{
  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Writing to hardware");

  writeCommandsToHardware();

  for (auto i = 0u; i < hw_commands_.size(); i++)
  {
    // Simulate sending commands to the hardware
    RCLCPP_INFO(
      rclcpp::get_logger(HW_NAME), "Got velocity command %.5f for '%s'!", hw_commands_[i],
      info_.joints[i].name.c_str());
  }

  RCLCPP_INFO(rclcpp::get_logger(HW_NAME), "Joints successfully written!");

  return hardware_interface::return_type::OK;
}

}  // namespace husky_base

#include "pluginlib/class_list_macros.hpp"
PLUGINLIB_EXPORT_CLASS(
  husky_base::HuskyBase, hardware_interface::SystemInterface)
