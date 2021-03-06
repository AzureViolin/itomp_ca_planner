/*

License

ITOMP Optimization-based Planner
Copyright © and trademark ™ 2014 University of North Carolina at Chapel Hill.
All rights reserved.

Permission to use, copy, modify, and distribute this software and its documentation
for educational, research, and non-profit purposes, without fee, and without a
written agreement is hereby granted, provided that the above copyright notice,
this paragraph, and the following four paragraphs appear in all copies.

This software program and documentation are copyrighted by the University of North
Carolina at Chapel Hill. The software program and documentation are supplied "as is,"
without any accompanying services from the University of North Carolina at Chapel
Hill or the authors. The University of North Carolina at Chapel Hill and the
authors do not warrant that the operation of the program will be uninterrupted
or error-free. The end-user understands that the program was developed for research
purposes and is advised not to rely exclusively on the program for any reason.

IN NO EVENT SHALL THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL OR THE AUTHORS
BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
DOCUMENTATION, EVEN IF THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL OR THE
AUTHORS HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL AND THE AUTHORS SPECIFICALLY
DISCLAIM ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE AND ANY STATUTORY WARRANTY
OF NON-INFRINGEMENT. THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND
THE UNIVERSITY OF NORTH CAROLINA AT CHAPEL HILL AND THE AUTHORS HAVE NO OBLIGATIONS
TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

Any questions or comments should be sent to the author chpark@cs.unc.edu

*/

#include <pluginlib/class_loader.h>
#include <ros/ros.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/planning_interface/planning_interface.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit_msgs/DisplayTrajectory.h>
#include <moveit_msgs/DisplayRobotState.h>
#include <moveit_msgs/PlanningScene.h>
#include <moveit_msgs/PositionConstraint.h>
#include <moveit_msgs/OrientationConstraint.h>
#include <boost/variant/get.hpp>
#include <boost/lexical_cast.hpp>
#include <geometric_shapes/mesh_operations.h>
#include <geometric_shapes/shape_operations.h>
#include <geometric_shapes/shapes.h>
#include <move_kuka/move_kuka_test.h>
#include <fstream>

using namespace std;

const int M = 8;

namespace move_kuka
{

MoveKukaTest::MoveKukaTest(const ros::NodeHandle& node_handle) :
		node_handle_(node_handle)
{

}

MoveKukaTest::~MoveKukaTest()
{
}

void MoveKukaTest::run(const std::string& group_name)
{
	// scene initialization

	robot_model_loader::RobotModelLoader robot_model_loader(
			"robot_description");
	robot_model_ = robot_model_loader.getModel();
	planning_scene_.reset(new planning_scene::PlanningScene(robot_model_));
	planning_scene_diff_publisher_ = node_handle_.advertise<
			moveit_msgs::PlanningScene>("/planning_scene", 1);
	while (planning_scene_diff_publisher_.getNumSubscribers() < 1)
	{
		ros::WallDuration sleep_t(0.5);
		sleep_t.sleep();
		ROS_INFO("Waiting planning_scene subscribers");
	}

	loadStaticScene();

	collision_detection::AllowedCollisionMatrix& acm =
			planning_scene_->getAllowedCollisionMatrixNonConst();
	acm.setEntry("environment", "segment_00", true);
	acm.setEntry("environment", "segment_0", true);
	acm.setEntry("environment", "segment_1", true);

	// planner initialization

	group_name_ = group_name;

	boost::scoped_ptr<pluginlib::ClassLoader<planning_interface::PlannerManager> > planner_plugin_loader;
	std::string planner_plugin_name;
	if (!node_handle_.getParam("planning_plugin", planner_plugin_name))
		ROS_FATAL_STREAM("Could not find planner plugin name");
	try
	{
		planner_plugin_loader.reset(
				new pluginlib::ClassLoader<planning_interface::PlannerManager>(
						"moveit_core", "planning_interface::PlannerManager"));
	} catch (pluginlib::PluginlibException& ex)
	{
		ROS_FATAL_STREAM(
				"Exception while creating planning plugin loader " << ex.what());
	}

	try
	{
		itomp_planner_instance_.reset(
				planner_plugin_loader->createUnmanagedInstance(
						planner_plugin_name));
		if (!itomp_planner_instance_->initialize(robot_model_,
				node_handle_.getNamespace()))
			ROS_FATAL_STREAM("Could not initialize planner instance");
		ROS_INFO_STREAM(
				"Using planning interface '" << itomp_planner_instance_->getDescription() << "'");
	} catch (pluginlib::PluginlibException& ex)
	{
		const std::vector<std::string> &classes =
				planner_plugin_loader->getDeclaredClasses();
		std::stringstream ss;
		for (std::size_t i = 0; i < classes.size(); ++i)
			ss << classes[i] << " ";
		ROS_ERROR_STREAM(
				"Exception while loading planner '" << planner_plugin_name << "': " << ex.what() << std::endl << "Available plugins: " << ss.str());
	}

	display_publisher_ = node_handle_.advertise<moveit_msgs::DisplayTrajectory>(
			"/move_group/display_planned_path", 1, true);
	vis_marker_array_publisher_ = node_handle_.advertise<
			visualization_msgs::MarkerArray>("visualization_marker_array", 100,
			true);

	ros::WallDuration sleep_time(0.01);
	sleep_time.sleep();

	///////////////////////////////////////////////////////

	moveit_msgs::DisplayTrajectory display_trajectory;
	moveit_msgs::MotionPlanResponse response;
	planning_interface::MotionPlanRequest req;
	planning_interface::MotionPlanResponse res;

	// Set start / goal states
	robot_state::RobotState& start_state =
			planning_scene_->getCurrentStateNonConst();
	std::vector<robot_state::RobotState> goal_states;
	goal_states.resize(10, planning_scene_->getCurrentStateNonConst());
	initStartGoalStates(start_state, goal_states);

	// trajectory optimization using ITOMP
	plan(req, res, start_state, goal_states);
	res.getMessage(response);

	// display trajectories
	display_trajectory.trajectory_start = response.trajectory_start;
	display_trajectory.trajectory.push_back(response.trajectory);
	display_publisher_.publish(display_trajectory);

	// clean up
	itomp_planner_instance_.reset();
	planning_scene_.reset();
	robot_model_.reset();

	sleep_time.sleep();
	ROS_INFO("Done");
}

void MoveKukaTest::initStartGoalStates(robot_state::RobotState& start_state,
		std::vector<robot_state::RobotState>& goal_states)
{
	std::map<std::string, double> values;
	const robot_state::JointModelGroup* joint_model_group =
			start_state.getJointModelGroup(group_name_);

	std::vector<robot_state::RobotState> states(2, start_state);
	const double INV_SQRT_2 = 1.0 / sqrt(2.0);
	double EE_CONSTRAINTS[][7] =
	{
	{ .2, .10, 1.2, -0.5, 0.5, -0.5, 0.5 },
	{ .15, .2, .85 + .1, -INV_SQRT_2, 0, 0, INV_SQRT_2 }, };

	Eigen::Affine3d goal_transform[2];
	Eigen::Affine3d transform_1_inv =
			robot_model_->getLinkModel("tcp_1_link")->getJointOriginTransform().inverse();

	for (int i = 0; i < 2; ++i)
	{
		EE_CONSTRAINTS[i][0] -= 5.4 * 0.1;
		EE_CONSTRAINTS[i][1] -= 1.9 * 0.1;
		EE_CONSTRAINTS[i][2] -= 4.16 * 0.1;

		EE_CONSTRAINTS[i][0] = -EE_CONSTRAINTS[i][0];
		EE_CONSTRAINTS[i][1] = -EE_CONSTRAINTS[i][1];

		EE_CONSTRAINTS[i][0] += 0.3;

		Eigen::Vector3d pos(EE_CONSTRAINTS[i][0], EE_CONSTRAINTS[i][1],
				EE_CONSTRAINTS[i][2]);
		drawEndeffectorPosition(i, pos);

		Eigen::Vector3d trans = Eigen::Vector3d(EE_CONSTRAINTS[i][0],
				EE_CONSTRAINTS[i][1], EE_CONSTRAINTS[i][2]);
		Eigen::Quaterniond rot = Eigen::Quaterniond(EE_CONSTRAINTS[i][6],
				EE_CONSTRAINTS[i][3], EE_CONSTRAINTS[i][4],
				EE_CONSTRAINTS[i][5]);

		goal_transform[i].linear() = rot.toRotationMatrix();
		goal_transform[i].translation() = trans;

		goal_transform[i] = goal_transform[i] * transform_1_inv;

		states[i].update();
		computeIKState(states[i], goal_transform[i]);
	}

	start_state = states[0];
	robot_state::RobotState& goal_state = states[1];

	renderStartGoalStates(start_state, goal_state);

	for (int j = 0; j < goal_states.size(); ++j)
	{
		computeIKState(goal_states[j], goal_transform[1], true);
	}
}

bool MoveKukaTest::isStateSingular(robot_state::RobotState& state)
{
	// check singularity
	Eigen::MatrixXd jacobianFull = (state.getJacobian(
			planning_scene_->getRobotModel()->getJointModelGroup(group_name_)));
	Eigen::JacobiSVD<Eigen::MatrixXd> svd(jacobianFull);
	int rows = svd.singularValues().rows();
	double min_value = svd.singularValues()(rows - 1);

	const double threshold = 1e-3;
	if (min_value < threshold)
		return true;
	else
		return false;
}

void MoveKukaTest::plan(planning_interface::MotionPlanRequest& req,
		planning_interface::MotionPlanResponse& res,
		const robot_state::RobotState& start_state,
		std::vector<robot_state::RobotState>& goal_states)
{
	const robot_state::JointModelGroup* joint_model_group =
			start_state.getJointModelGroup(group_name_);
	req.group_name = group_name_;
	req.allowed_planning_time = 3000.0;

	// Copy from start_state to req.start_state
	unsigned int num_joints = start_state.getVariableCount();
	req.start_state.joint_state.name = start_state.getVariableNames();
	req.start_state.joint_state.position.resize(num_joints);
	req.start_state.joint_state.velocity.resize(num_joints);
	req.start_state.joint_state.effort.resize(num_joints);
	memcpy(&req.start_state.joint_state.position[0],
			start_state.getVariablePositions(), sizeof(double) * num_joints);
	if (start_state.hasVelocities())
		memcpy(&req.start_state.joint_state.velocity[0],
				start_state.getVariableVelocities(),
				sizeof(double) * num_joints);
	else
		memset(&req.start_state.joint_state.velocity[0], 0,
				sizeof(double) * num_joints);
	if (start_state.hasAccelerations())
		memcpy(&req.start_state.joint_state.effort[0],
				start_state.getVariableAccelerations(),
				sizeof(double) * num_joints);
	else
		memset(&req.start_state.joint_state.effort[0], 0,
				sizeof(double) * num_joints);

	// goal state
	req.goal_constraints.clear();
	for (int i = 0; i < goal_states.size(); ++i)
	{
		moveit_msgs::Constraints joint_goal =
				kinematic_constraints::constructGoalConstraints(goal_states[i],
						joint_model_group);
		req.goal_constraints.push_back(joint_goal);
	}

	planning_interface::PlanningContextPtr context =
			itomp_planner_instance_->getPlanningContext(planning_scene_, req,
					res.error_code_);
	context->solve(res);
	if (res.error_code_.val != res.error_code_.SUCCESS)
	{
		ROS_ERROR("Could not compute plan successfully");
		return;
	}
}

void MoveKukaTest::loadStaticScene()
{
	moveit_msgs::PlanningScene planning_scene_msg;
	std::string environment_file;
	std::vector<double> environment_position;

	node_handle_.param<std::string>("/itomp_planner/environment_model",
			environment_file, "");

	if (!environment_file.empty())
	{
		double scale;
		node_handle_.param("/itomp_planner/environment_model_scale", scale,
				1.0);
		environment_position.resize(3, 0);
		if (node_handle_.hasParam("/itomp_planner/environment_model_position"))
		{
			XmlRpc::XmlRpcValue segment;
			node_handle_.getParam("/itomp_planner/environment_model_position",
					segment);
			if (segment.getType() == XmlRpc::XmlRpcValue::TypeArray)
			{
				int size = segment.size();
				for (int i = 0; i < size; ++i)
				{
					double value = segment[i];
					environment_position[i] = value;
				}
			}
		}

		// Collision object
		moveit_msgs::CollisionObject collision_object;
		collision_object.header.frame_id = robot_model_->getModelFrame();
		collision_object.id = "environment";
		geometry_msgs::Pose pose;
		pose.position.x = environment_position[0];
		pose.position.y = environment_position[1];
		pose.position.z = environment_position[2];
		pose.orientation.x = 0.0;
		pose.orientation.y = 0.0;
		pose.orientation.z = 0.0;
		pose.orientation.w = 1.0;

		shapes::Mesh* shape = shapes::createMeshFromResource(environment_file,
				Eigen::Vector3d(scale, scale, scale));
		shapes::ShapeMsg mesh_msg;
		shapes::constructMsgFromShape(shape, mesh_msg);
		shape_msgs::Mesh mesh = boost::get<shape_msgs::Mesh>(mesh_msg);

		collision_object.meshes.push_back(mesh);
		collision_object.mesh_poses.push_back(pose);

		collision_object.operation = collision_object.ADD;
		//moveit_msgs::PlanningScene planning_scene_msg;
		planning_scene_msg.world.collision_objects.push_back(collision_object);
		planning_scene_msg.is_diff = true;
		planning_scene_->setPlanningSceneDiffMsg(planning_scene_msg);
	}

	planning_scene_diff_publisher_.publish(planning_scene_msg);
}

void MoveKukaTest::renderStartGoalStates(robot_state::RobotState& start_state,
		robot_state::RobotState& goal_state)
{
	// display start / goal states
	int num_variables = start_state.getVariableNames().size();
	static ros::Publisher start_state_display_publisher =
			node_handle_.advertise<moveit_msgs::DisplayRobotState>(
					"/move_itomp/display_start_state", 1, true);
	moveit_msgs::DisplayRobotState disp_start_state;
	disp_start_state.state.joint_state.header.frame_id =
			robot_model_->getModelFrame();
	disp_start_state.state.joint_state.name = start_state.getVariableNames();
	disp_start_state.state.joint_state.position.resize(num_variables);
	memcpy(&disp_start_state.state.joint_state.position[0],
			start_state.getVariablePositions(), sizeof(double) * num_variables);
	disp_start_state.highlight_links.clear();
	const std::vector<std::string>& link_model_names =
			robot_model_->getLinkModelNames();
	for (unsigned int i = 0; i < link_model_names.size(); ++i)
	{
		std_msgs::ColorRGBA color;

		color.a = 0.5;
		color.r = 0.0;
		color.g = 1.0;
		color.b = 0.5;

		moveit_msgs::ObjectColor obj_color;
		obj_color.id = link_model_names[i];
		obj_color.color = color;
		disp_start_state.highlight_links.push_back(obj_color);
	}
	start_state_display_publisher.publish(disp_start_state);

	static ros::Publisher goal_state_display_publisher = node_handle_.advertise<
			moveit_msgs::DisplayRobotState>("/move_itomp/display_goal_state", 1,
			true);
	moveit_msgs::DisplayRobotState disp_goal_state;
	disp_goal_state.state.joint_state.header.frame_id =
			robot_model_->getModelFrame();
	disp_goal_state.state.joint_state.name = goal_state.getVariableNames();
	disp_goal_state.state.joint_state.position.resize(num_variables);
	memcpy(&disp_goal_state.state.joint_state.position[0],
			goal_state.getVariablePositions(), sizeof(double) * num_variables);
	disp_goal_state.highlight_links.clear();
	for (int i = 0; i < link_model_names.size(); ++i)
	{
		std_msgs::ColorRGBA color;
		color.a = 0.5;
		color.r = 0.0;
		color.g = 0.5;
		color.b = 1.0;
		moveit_msgs::ObjectColor obj_color;
		obj_color.id = link_model_names[i];
		obj_color.color = color;
		disp_goal_state.highlight_links.push_back(obj_color);
	}
	goal_state_display_publisher.publish(disp_goal_state);
}

bool MoveKukaTest::isStateCollide(const robot_state::RobotState& state)
{
	collision_detection::CollisionRequest collision_request;
	collision_detection::CollisionResult collision_result;
	collision_request.verbose = false;
	collision_request.contacts = false;

	planning_scene_->checkCollisionUnpadded(collision_request, collision_result,
			state);

	return collision_result.collision;
}

void MoveKukaTest::computeIKState(robot_state::RobotState& ik_state,
		const Eigen::Affine3d& end_effector_state, bool rand)
{
	// compute waypoint ik solutions

	const robot_state::JointModelGroup* joint_model_group =
			ik_state.getJointModelGroup(group_name_);

	kinematics::KinematicsQueryOptions options;
	options.return_approximate_solution = false;
	bool found_ik = false;

	robot_state::RobotState org_start(ik_state);
	int i = 0;

	if (rand)
		ik_state.setToRandomPositionsNearBy(joint_model_group, org_start,
				log(-3) / log(10));

	while (true)
	{
		found_ik = ik_state.setFromIK(joint_model_group, end_effector_state, 10,
				0.1, moveit::core::GroupStateValidityCallbackFn(), options);
		ik_state.update();

		found_ik &= !isStateCollide(ik_state);

		if (found_ik && isStateSingular(ik_state))
			found_ik = false;

		if (found_ik)
			break;

		++i;

		double dist = log(-3 + 0.001 * i) / log(10);

		ik_state.setToRandomPositionsNearBy(joint_model_group, org_start, dist);
	}

	if (found_ik)
	{
		//ROS_INFO("IK solution found after %d trials", i + 1);
	}
	else
	{
		ROS_INFO("Could not find IK solution");
	}
}

void MoveKukaTest::drawEndeffectorPosition(int id,
		const Eigen::Vector3d& position)
{
	const double trajectory_color_diff = 0.33;
	const double scale = 0.02;
	const int marker_step = 1;

	visualization_msgs::Marker::_color_type BLUE, LIGHT_YELLOW;
	visualization_msgs::Marker::_color_type RED, LIGHT_RED;
	RED.a = 1.0;
	RED.r = 1.0;
	RED.g = 0.0;
	RED.b = 0.0;
	BLUE.a = 1.0;
	BLUE.r = 0.5;
	BLUE.g = 0.5;
	BLUE.b = 1.0;
	LIGHT_RED = RED;
	LIGHT_RED.g = 0.5;
	LIGHT_RED.b = 0.5;
	LIGHT_YELLOW = BLUE;
	LIGHT_YELLOW.b = 0.5;

	visualization_msgs::Marker msg;
	msg.header.frame_id = robot_model_->getModelFrame();
	msg.header.stamp = ros::Time::now();
	msg.ns = "cartesian_traj";
	msg.type = visualization_msgs::Marker::CUBE_LIST;
	msg.action = visualization_msgs::Marker::ADD;

	msg.scale.x = scale;
	msg.scale.y = scale;
	msg.scale.z = scale;

	msg.id = id;
	msg.color = BLUE;

	msg.points.resize(0);
	geometry_msgs::Point point;
	point.x = position(0);
	point.y = position(1);
	point.z = position(2);
	msg.points.push_back(point);

	visualization_msgs::MarkerArray ma;
	ma.markers.push_back(msg);
	vis_marker_array_publisher_.publish(ma);
}

void MoveKukaTest::drawPath(int id, const Eigen::Vector3d& from,
		const Eigen::Vector3d& to)
{
	const double trajectory_color_diff = 0.33;
	const double scale = 0.005;
	const int marker_step = 1;

	visualization_msgs::Marker::_color_type BLUE, LIGHT_YELLOW;
	visualization_msgs::Marker::_color_type RED, LIGHT_RED;
	RED.a = 1.0;
	RED.r = 1.0;
	RED.g = 0.0;
	RED.b = 0.0;
	BLUE.a = 1.0;
	BLUE.r = 0.5;
	BLUE.g = 0.5;
	BLUE.b = 1.0;
	LIGHT_RED = RED;
	LIGHT_RED.g = 0.5;
	LIGHT_RED.b = 0.5;
	LIGHT_YELLOW = BLUE;
	LIGHT_YELLOW.b = 0.5;

	visualization_msgs::Marker msg;
	msg.header.frame_id = robot_model_->getModelFrame();
	msg.header.stamp = ros::Time::now();
	msg.ns = "cartesian_traj";
	msg.type = visualization_msgs::Marker::LINE_LIST;
	msg.action = visualization_msgs::Marker::ADD;

	msg.scale.x = scale;
	msg.scale.y = scale;
	msg.scale.z = scale;

	msg.id = id;
	msg.color = BLUE;

	msg.points.resize(0);
	geometry_msgs::Point point;
	point.x = from(0) - 0.001;
	point.y = from(1);
	point.z = from(2);
	msg.points.push_back(point);
	point.x = to(0) - 0.001;
	point.y = to(1);
	point.z = to(2);
	msg.points.push_back(point);

	visualization_msgs::MarkerArray ma;
	ma.markers.push_back(msg);
	vis_marker_array_publisher_.publish(ma);
}

}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "move_itomp");
	ros::AsyncSpinner spinner(1);
	spinner.start();
	ros::NodeHandle node_handle("~");

	move_kuka::MoveKukaTest* move_kuka = new move_kuka::MoveKukaTest(
			node_handle);
	move_kuka->run("lower_body");
	delete move_kuka;

	return 0;
}
