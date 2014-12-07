// Original code from pr2_moveit_tutorials::motion_planning_api_tutorial.cpp
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
#include <move_kuka/move_kuka.h>
#include <fstream>

using namespace std;

const int M = 8;

namespace move_kuka
{

MoveKuka::MoveKuka(const ros::NodeHandle& node_handle) :
		node_handle_(node_handle)
{

}

MoveKuka::~MoveKuka()
{

}

void MoveKuka::run(const std::string& group_name)
{

	group_name_ = group_name;

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

	collision_detection::AllowedCollisionMatrix& acm =
			planning_scene_->getAllowedCollisionMatrixNonConst();
	acm.setEntry("environment", "segment_00", true);
	acm.setEntry("environment", "segment_0", true);
	acm.setEntry("environment", "segment_1", true);

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

	planner_plugin_name = "ompl_interface/OMPLPlanner";
	try
	{
		ompl_planner_instance_.reset(
				planner_plugin_loader->createUnmanagedInstance(
						planner_plugin_name));
		if (!ompl_planner_instance_->initialize(robot_model_,
				node_handle_.getNamespace()))
			ROS_FATAL_STREAM("Could not initialize planner instance");
		ROS_INFO_STREAM(
				"Using planning interface '" << ompl_planner_instance_->getDescription() << "'");
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

	loadStaticScene();

	ros::WallDuration sleep_time(0.01);
	sleep_time.sleep();

	///////////////////////////////////////////////////////

	moveit_msgs::DisplayTrajectory display_trajectory;
	moveit_msgs::MotionPlanResponse response;
	visualization_msgs::MarkerArray ma;
	planning_interface::MotionPlanRequest req, req2;
	planning_interface::MotionPlanResponse res;

	req.workspace_parameters.min_corner.x =
			req.workspace_parameters.min_corner.y =
					req.workspace_parameters.min_corner.z = -10.0;
	req.workspace_parameters.max_corner.x =
			req.workspace_parameters.max_corner.y =
					req.workspace_parameters.max_corner.z = 10.0;

	// Set start_state
	std::map<std::string, double> values;
	robot_state::RobotState& start_state =
			planning_scene_->getCurrentStateNonConst();
	const robot_state::JointModelGroup* joint_model_group =
			start_state.getJointModelGroup(group_name_);

	joint_model_group->getVariableDefaultPositions("idle", values);
	start_state.setVariablePositions(values);
	start_state.update();

	// Setup a goal state
	robot_state::RobotState goal_state(start_state);
	joint_model_group->getVariableDefaultPositions("idle", values);
	goal_state.setVariablePositions(values);
	goal_state.update();

	renderPRMGraph(start_state);
	sleep_time.sleep();

	const double INV_SQRT_2 = 1.0 / sqrt(2.0);

	int benchmark = (group_name_ == "lower_body") ? 1 : 2;

	for (int trial = 0; trial < 1; ++trial)
	{
		display_trajectory.trajectory.clear();

		if (benchmark == 1)
		{
			// static benchmark 1
			///

			bool wait_at_waypoints = false;

			double EE_CONSTRAINTS[][7] =
			{
			{ .2, .05, 1.2, -0.5, 0.5, -0.5, 0.5 },
			{ .2, .2, .85 + .1, -INV_SQRT_2, 0, 0, INV_SQRT_2 },
			{ .2, .10, 1.2, -0.5, 0.5, -0.5, 0.5 },
			{ .15, .2, .85 + .1, -INV_SQRT_2, 0, 0, INV_SQRT_2 },
			{ .2, .15, 1.2, -0.5, 0.5, -0.5, 0.5 },
			{ .1, .2, .85 + .1, -INV_SQRT_2, 0, 0, INV_SQRT_2 }, };

			for (int i = 0; i < 6; ++i)
			{
				for (int j = 0; j < 3; ++j)
					EE_CONSTRAINTS[i][j] *= 1.0;

				EE_CONSTRAINTS[i][0] -= 5.4 * 0.1;
				EE_CONSTRAINTS[i][1] -= 1.9 * 0.1;
				EE_CONSTRAINTS[i][2] -= 4.16 * 0.1;

				EE_CONSTRAINTS[i][0] = -EE_CONSTRAINTS[i][0];
				EE_CONSTRAINTS[i][1] = -EE_CONSTRAINTS[i][1];
			}

			for (int i = 0; i < 6; ++i)
				EE_CONSTRAINTS[i][0] += 0.3;

			for (int i = 0; i < 6; ++i)
			{
				Eigen::Vector3d pos(EE_CONSTRAINTS[i][0], EE_CONSTRAINTS[i][1],
						EE_CONSTRAINTS[i][2]);
				drawEndeffectorPosition(i, pos);
				ROS_INFO(
						"effector pos %d: %f %f %f", i, EE_CONSTRAINTS[i][0], EE_CONSTRAINTS[i][1], EE_CONSTRAINTS[i][2]);
			}
			sleep_time.sleep();

			Eigen::Affine3d goal_transform[6];
			for (int i = 0; i < 6; ++i)
			{
				Eigen::Vector3d trans = Eigen::Vector3d(EE_CONSTRAINTS[i][0],
						EE_CONSTRAINTS[i][1], EE_CONSTRAINTS[i][2]);
				Eigen::Quaterniond rot = Eigen::Quaterniond(
						EE_CONSTRAINTS[i][6], EE_CONSTRAINTS[i][3],
						EE_CONSTRAINTS[i][4], EE_CONSTRAINTS[i][5]);

				goal_transform[i].linear() = rot.toRotationMatrix();
				goal_transform[i].translation() = trans;
			}

			// transform from tcp to arm end-effector
			Eigen::Affine3d transform_1_inv = robot_model_->getLinkModel(
					"tcp_1_link")->getJointOriginTransform().inverse();
			Eigen::Affine3d transform_2_inv = robot_model_->getLinkModel(
					"tcp_2_link")->getJointOriginTransform().inverse();
			Eigen::Affine3d transform_3_inv = robot_model_->getLinkModel(
					"tcp_3_link")->getJointOriginTransform().inverse();

			for (int i = 0; i < 6; ++i)
			{

				goal_transform[i] = goal_transform[i]
						* ((i % 2 == 0) ? transform_1_inv : transform_2_inv);

				//goal_transform[i] = goal_transform[i] * transform_3_inv;
			}

			start_state.update();
			ROS_ASSERT(isStateCollide(start_state) == false);

			std::vector<robot_state::RobotState> states(6, start_state);
			for (int i = 0; i < 6; ++i)
			{
				states[i].update();
				computeIKState(states[i], goal_transform[i]);
			}

			for (int i = 0; i < 6; ++i)
			{
				ROS_INFO("*** Planning Sequence %d ***", i);

				robot_state::RobotState& from_state = states[i];
				robot_state::RobotState& to_state = states[(i + 1) % 6];

				displayStates(from_state, to_state);
				sleep_time.sleep();

				if (i == 0 && wait_at_waypoints)
				{
					req2.trajectory_constraints.constraints.clear();
					plan(req2, res, from_state, from_state);
					res.getMessage(response);

					display_trajectory.trajectory_start =
							response.trajectory_start;
					response.trajectory.joint_trajectory.points.resize(
							response.trajectory.joint_trajectory.points.size()
									/ 5);
					display_trajectory.trajectory.push_back(
							response.trajectory);
				}

				const Eigen::Affine3d& transform = goal_transform[(i + 1) % 6];
				Eigen::Vector3d trans = transform.translation();
				Eigen::Quaterniond rot = Eigen::Quaterniond(transform.linear());

				geometry_msgs::PoseStamped goal_pose;
				goal_pose.header.frame_id = robot_model_->getModelFrame();
				goal_pose.pose.position.x = trans(0);
				goal_pose.pose.position.y = trans(1);
				goal_pose.pose.position.z = trans(2);
				goal_pose.pose.orientation.x = rot.x();
				goal_pose.pose.orientation.y = rot.y();
				goal_pose.pose.orientation.z = rot.z();
				goal_pose.pose.orientation.w = rot.w();
				std::string endeffector_name = "end_effector_link";

				req2.trajectory_constraints.constraints.clear();
				int traj_constraint_begin = 0;
				for (int c = 0; c < M; ++c)
				{

					// planning using OMPL
					ros::WallTime start_time = ros::WallTime::now();
					plan(req, res, from_state, goal_pose, endeffector_name);
					if (c == 0 && i == 0 && trial == 0)
						ROS_INFO(
								"PRM construction took %f sec", (ros::WallTime::now() - start_time).toSec());

					if (res.error_code_.val != res.error_code_.SUCCESS)
					{
						--c;
						continue;
					}
					res.getMessage(response);

					//printTrajectory(response.trajectory);

					// plan using ITOMP
					// use the last configuration of prev trajectory
					if (i != 5)
					{
						int num_joints = from_state.getVariableCount();
						std::vector<double> positions(num_joints);
						const robot_state::RobotState& last_state =
								res.trajectory_->getLastWayPoint();
						to_state.setVariablePositions(
								last_state.getVariablePositions());
						to_state.update();
					}

					moveit_msgs::JointConstraint jc;
					int num_joints =
							response.trajectory.joint_trajectory.points[0].positions.size();
					int num_points =
							response.trajectory.joint_trajectory.points.size();
					req2.trajectory_constraints.constraints.resize(
							traj_constraint_begin + num_points);
					std::string trajectory_index_string = boost::lexical_cast<
							std::string>(c);
					for (int j = 0; j < num_points; ++j)
					{
						int point = j + traj_constraint_begin;
						if (j == 0)
							req2.trajectory_constraints.constraints[point].name =
									trajectory_index_string;
						if (j == num_points - 1)
							req2.trajectory_constraints.constraints[point].name =
									"end";

						req2.trajectory_constraints.constraints[point].joint_constraints.resize(
								num_joints);
						for (int k = 0; k < num_joints; ++k)
						{
							jc.joint_name = from_state.getVariableNames()[k];
							jc.position =
									response.trajectory.joint_trajectory.points[j].positions[k];
							req2.trajectory_constraints.constraints[point].joint_constraints[k] =
									jc;
						}
					}
					traj_constraint_begin += num_points;
				}

				//req2.trajectory_constraints.constraints.clear();
				plan(req2, res, from_state, to_state);
				res.getMessage(response);

				int n = res.trajectory_->getWayPointCount();
				for (int k = 0; k < n; ++k)
				{
					bool is_collide = isStateCollide(
							res.trajectory_->getWayPoint(k));
					if (is_collide)
						ROS_INFO("%d waypoint has collision", k);
				}

				if (i == 0 && wait_at_waypoints == false)
					display_trajectory.trajectory_start =
							response.trajectory_start;
				display_trajectory.trajectory.push_back(response.trajectory);

				if (wait_at_waypoints)
				{
					req2.trajectory_constraints.constraints.clear();
					plan(req2, res, to_state, to_state);
					res.getMessage(response);
					response.trajectory.joint_trajectory.points.resize(
							response.trajectory.joint_trajectory.points.size()
									/ 5);
					display_trajectory.trajectory.push_back(
							response.trajectory);
				}
			}
			///
		}
		else if (benchmark == 2)
		{
			const int num_waypoints = 4;
			// static benchmark 2

			bool wait_at_waypoints = false;
			///
			double EE_CONSTRAINTS[][7] =
			{
			{ 3.3, 4, 7.0, 0.5, 0.5, 0.5, 0.5 },
			{ 3.3, 4, 10.0, 0.5, 0.5, 0.5, 0.5 },
			{ 3.3, 0, 10.0, 0.5, 0.5, 0.5, 0.5 },
			{ 3.3, 0, 7, 0.5, 0.5, 0.5, 0.5 }, };
			// 0.5, 0.5, 0.5, 0.5 right
			// -0.5, 0.5, -0.5, 0.5 left
			// INV_SQRT_2, 0.0, INV_SQRT_2, 0.0 bottom

			for (int i = 0; i < num_waypoints; ++i)
			{
				EE_CONSTRAINTS[i][0] *= 0.1;
				EE_CONSTRAINTS[i][1] *= 0.1;
				EE_CONSTRAINTS[i][2] *= 0.1;
			}

			Eigen::Affine3d goal_transform[num_waypoints];
			for (int i = 0; i < num_waypoints; ++i)
			{
				Eigen::Vector3d trans = Eigen::Vector3d(EE_CONSTRAINTS[i][0],
						EE_CONSTRAINTS[i][1], EE_CONSTRAINTS[i][2]);
				Eigen::Quaterniond rot = Eigen::Quaterniond(
						EE_CONSTRAINTS[i][6], EE_CONSTRAINTS[i][3],
						EE_CONSTRAINTS[i][4], EE_CONSTRAINTS[i][5]);

				goal_transform[i].linear() = rot.toRotationMatrix();
				goal_transform[i].translation() = trans;
			}

			for (int i = 0; i < num_waypoints - 1; ++i)
				drawPath(i, goal_transform[i].translation(),
						goal_transform[i + 1].translation());
			ros::WallDuration sleep_t(0.001);
			sleep_t.sleep();

			start_state.update();
			ROS_ASSERT(isStateCollide(start_state) == false);

			std::vector<robot_state::RobotState> states(num_waypoints,
					start_state);
			for (int i = 0; i < num_waypoints; ++i)
			{
				states[i].update();
				computeIKState(states[i], goal_transform[i]);
			}

			//for (int i = 0; i < 1; ++i)
			for (int i = 0; i < num_waypoints - 1; ++i)
			{
				ROS_INFO("*** Planning Sequence %d ***", i);

				robot_state::RobotState from_state = states[i];
				robot_state::RobotState& to_state = states[(i + 1)
						% num_waypoints];

				// use the last configuration of prev trajectory
				if (i != 0)
				{
					int num_joints = from_state.getVariableCount();
					std::vector<double> positions(num_joints);
					const robot_state::RobotState& last_state =
							res.trajectory_->getLastWayPoint();
					from_state.setVariablePositions(
							last_state.getVariablePositions());
					from_state.update();
				}

				displayStates(from_state, to_state);
				sleep_time.sleep();

				if (i == 0 && wait_at_waypoints)
				{

					req2.trajectory_constraints.constraints.clear();
					plan(req2, res, from_state, from_state);
					res.getMessage(response);

					display_trajectory.trajectory_start =
							response.trajectory_start;
					response.trajectory.joint_trajectory.points.resize(
							response.trajectory.joint_trajectory.points.size()
									/ 5);
					display_trajectory.trajectory.push_back(
							response.trajectory);
				}

				const Eigen::Affine3d& transform = goal_transform[(i + 1)
						% num_waypoints];
				Eigen::Vector3d trans = transform.translation();
				Eigen::Quaterniond rot = Eigen::Quaterniond(transform.linear());

				geometry_msgs::PoseStamped goal_pose;
				goal_pose.header.frame_id = robot_model_->getModelFrame();
				goal_pose.pose.position.x = trans(0);
				goal_pose.pose.position.y = trans(1);
				goal_pose.pose.position.z = trans(2);
				goal_pose.pose.orientation.x = rot.x();
				goal_pose.pose.orientation.y = rot.y();
				goal_pose.pose.orientation.z = rot.z();
				goal_pose.pose.orientation.w = rot.w();
				std::string endeffector_name = "tcp_2_link"; //"end_effector_link";

				req2.trajectory_constraints.constraints.clear();
				int traj_constraint_begin = 0;
				for (int c = 0; c < M; ++c)
				{
					// planning using OMPL
					ros::WallTime start_time = ros::WallTime::now();
					plan(req, res, from_state, goal_pose, endeffector_name);
					if (c == 0 && i == 0 && trial == 0)
						ROS_INFO(
								"PRM construction took %f sec", (ros::WallTime::now() - start_time).toSec());
					if (res.error_code_.val != res.error_code_.SUCCESS)
					{
						--c;
						continue;
					}
					res.getMessage(response);

					//printTrajectory(response.trajectory);

					moveit_msgs::JointConstraint jc;
					int num_joints =
							response.trajectory.joint_trajectory.points[0].positions.size();
					int num_points =
							response.trajectory.joint_trajectory.points.size();
					req2.trajectory_constraints.constraints.resize(
							traj_constraint_begin + num_points);
					std::string trajectory_index_string = boost::lexical_cast<
							std::string>(c);
					for (int j = 0; j < num_points; ++j)
					{
						int point = j + traj_constraint_begin;
						if (j == 0)
							req2.trajectory_constraints.constraints[point].name =
									trajectory_index_string;
						if (j == num_points - 1)
							req2.trajectory_constraints.constraints[point].name =
									"end";

						req2.trajectory_constraints.constraints[point].joint_constraints.resize(
								num_joints);
						for (int k = 0; k < num_joints; ++k)
						{
							jc.joint_name = from_state.getVariableNames()[k];
							jc.position =
									response.trajectory.joint_trajectory.points[j].positions[k];
							req2.trajectory_constraints.constraints[point].joint_constraints[k] =
									jc;
						}
					}
					traj_constraint_begin += num_points;
				}

				//req2.trajectory_constraints.constraints.clear();
				moveit_msgs::PositionConstraint pc;
				pc.target_point_offset.x = EE_CONSTRAINTS[i][0];
				pc.target_point_offset.y = EE_CONSTRAINTS[i][1];
				pc.target_point_offset.z = EE_CONSTRAINTS[i][2];
				req2.path_constraints.position_constraints.push_back(pc);
				pc.target_point_offset.x = EE_CONSTRAINTS[i + 1][0];
				pc.target_point_offset.y = EE_CONSTRAINTS[i + 1][1];
				pc.target_point_offset.z = EE_CONSTRAINTS[i + 1][2];
				req2.path_constraints.position_constraints.push_back(pc);
				moveit_msgs::OrientationConstraint oc;
				oc.orientation.x = EE_CONSTRAINTS[i][3];
				oc.orientation.y = EE_CONSTRAINTS[i][4];
				oc.orientation.z = EE_CONSTRAINTS[i][5];
				oc.orientation.w = EE_CONSTRAINTS[i][6];
				req2.path_constraints.orientation_constraints.push_back(oc);
				plan(req2, res, from_state, to_state);
				req2.path_constraints.position_constraints.clear();
				req2.path_constraints.orientation_constraints.clear();
				res.getMessage(response);

				int n = res.trajectory_->getWayPointCount();
				for (int k = 0; k < n; ++k)
				{
					bool is_collide = isStateCollide(
							res.trajectory_->getWayPoint(k));
					if (is_collide)
						ROS_INFO("%d waypoint has collision", k);
				}

				if (i == 0 && !wait_at_waypoints)
				{
					display_trajectory.trajectory_start =
							response.trajectory_start;
				}
				display_trajectory.trajectory.push_back(response.trajectory);

				/*
				 req2.trajectory_constraints.constraints.clear();
				 plan(req2, res, to_state, to_state);
				 res.getMessage(response);
				 response.trajectory.joint_trajectory.points.resize(
				 response.trajectory.joint_trajectory.points.size() / 5);
				 display_trajectory.trajectory.push_back(response.trajectory);
				 */
			}
			///
		}

		// publish trajectory

		// reduce waypoints
		/*
		 for (int j = 0; j < display_trajectory.trajectory.size(); ++j)
		 {
		 std::vector<trajectory_msgs::JointTrajectoryPoint> short_traj;
		 for (int i = 0; i < display_trajectory.trajectory[j].joint_trajectory.points.size(); i += 51)
		 short_traj.push_back(display_trajectory.trajectory[j].joint_trajectory.points[i]);
		 display_trajectory.trajectory[j].joint_trajectory.points = short_traj;
		 }
		 */

		display_publisher_.publish(display_trajectory);

		/*
		 for (int j = 0; j < display_trajectory.trajectory.size(); ++j)
		 {
		 for (int i = 0;
		 i
		 < display_trajectory.trajectory[j].joint_trajectory.points.size();
		 ++i)
		 printf("%d %d : %f\n", j, i,
		 display_trajectory.trajectory[j].joint_trajectory.points[i].time_from_start.toSec());
		 }
		 */
		int num_trajectories = display_trajectory.trajectory.size();
		for (int i = 0; i < num_trajectories; ++i)
			printTrajectory(display_trajectory.trajectory[i]);

	}

	itomp_planner_instance_.reset();
	ompl_planner_instance_.reset();
	planning_scene_.reset();
	robot_model_.reset();

	sleep_time.sleep();
	ROS_INFO("Done");
}

bool MoveKuka::isStateSingular(robot_state::RobotState& state)
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

void MoveKuka::plan(planning_interface::MotionPlanRequest& req,
		planning_interface::MotionPlanResponse& res,
		robot_state::RobotState& start_state,
		robot_state::RobotState& goal_state)
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
	moveit_msgs::Constraints joint_goal =
			kinematic_constraints::constructGoalConstraints(goal_state,
					joint_model_group);
	req.goal_constraints.clear();
	req.goal_constraints.push_back(joint_goal);

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

void MoveKuka::plan(planning_interface::MotionPlanRequest& req,
		planning_interface::MotionPlanResponse& res,
		robot_state::RobotState& start_state,
		geometry_msgs::PoseStamped& goal_pose,
		const std::string& endeffector_link)
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

	planning_scene_->getCurrentStateNonConst().update();

	// goal state
	std::vector<double> tolerance_pose(3, 0.0001);
	std::vector<double> tolerance_angle(3, 0.01);
	moveit_msgs::Constraints pose_goal =
			kinematic_constraints::constructGoalConstraints(endeffector_link,
					goal_pose, tolerance_pose, tolerance_angle);
	req.goal_constraints.clear();
	req.goal_constraints.push_back(pose_goal);

	moveit_msgs::OrientationConstraint oc;
	oc.link_name = "tool";
	oc.orientation.x = -0.5;
	oc.orientation.y = -0.5;
	oc.orientation.z = 0.5;
	oc.orientation.w = 0.5;
	oc.absolute_x_axis_tolerance = M_PI;
	oc.absolute_y_axis_tolerance = M_PI;
	oc.absolute_z_axis_tolerance = 5.0 * M_PI / 180.0;
	oc.weight = 1.0;
	oc.header.frame_id = robot_model_->getModelFrame();
	oc.header.stamp = ros::Time::now();
	//req.path_constraints.orientation_constraints.push_back(oc);

	planning_interface::PlanningContextPtr context =
			ompl_planner_instance_->getPlanningContext(planning_scene_, req,
					res.error_code_);
	context->solve(res);
	if (res.error_code_.val != res.error_code_.SUCCESS)
	{
		ROS_ERROR("Could not compute plan successfully");
		return;
	}
}

void MoveKuka::loadStaticScene()
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

void MoveKuka::displayState(robot_state::RobotState& state)
{
	std_msgs::ColorRGBA color;
	color.a = 0.5;
	color.r = 1.0;
	color.g = 0.5;
	color.b = 0.5;

	int num_variables = state.getVariableNames().size();
	static ros::Publisher state_display_publisher = node_handle_.advertise<
			moveit_msgs::DisplayRobotState>("/move_itomp/display_state", 1,
			true);
	moveit_msgs::DisplayRobotState disp_state;
	disp_state.state.joint_state.header.frame_id =
			robot_model_->getModelFrame();
	disp_state.state.joint_state.name = state.getVariableNames();
	disp_state.state.joint_state.position.resize(num_variables);
	memcpy(&disp_state.state.joint_state.position[0],
			state.getVariablePositions(), sizeof(double) * num_variables);
	disp_state.highlight_links.clear();
	const std::vector<std::string>& link_model_names =
			robot_model_->getLinkModelNames();
	for (unsigned int i = 0; i < link_model_names.size(); ++i)
	{
		moveit_msgs::ObjectColor obj_color;
		obj_color.id = link_model_names[i];
		obj_color.color = color;
		disp_state.highlight_links.push_back(obj_color);
	}
	state_display_publisher.publish(disp_state);
}

void MoveKuka::displayStates(robot_state::RobotState& start_state,
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
		/*
		 color.a = 1.0;
		 color.r = 1.0;
		 color.g = 0.333;
		 color.b = 0.0;
		 */
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

bool MoveKuka::isStateCollide(const robot_state::RobotState& state)
{
	visualization_msgs::MarkerArray ma;
	visualization_msgs::Marker msg;
	msg.header.frame_id = robot_model_->getModelFrame();
	msg.header.stamp = ros::Time::now();
	msg.ns = "collision";
	msg.type = visualization_msgs::Marker::SPHERE_LIST;
	msg.action = visualization_msgs::Marker::ADD;
	msg.scale.x = 0.02;
	msg.scale.y = 0.02;
	msg.scale.z = 0.02;
	msg.color.a = 1.0;
	msg.color.r = 1.0;
	msg.color.g = 1.0;
	msg.color.b = 0.0;
	msg.id = 0;

	collision_detection::CollisionRequest collision_request;
	collision_detection::CollisionResult collision_result;
	collision_request.verbose = true;
	collision_request.contacts = true;

	planning_scene_->checkCollisionUnpadded(collision_request, collision_result,
			state);

	const collision_detection::CollisionResult::ContactMap& contact_map =
			collision_result.contacts;
	for (collision_detection::CollisionResult::ContactMap::const_iterator it =
			contact_map.begin(); it != contact_map.end(); ++it)
	{
		for (int i = 0; i < it->second.size(); ++i)
		{
			const collision_detection::Contact& contact = it->second[i];
			geometry_msgs::Point point;
			point.x = contact.pos(0);
			point.y = contact.pos(1);
			point.z = contact.pos(2);
			msg.points.push_back(point);
		}
	}
	ma.markers.push_back(msg);
	vis_marker_array_publisher_.publish(ma);
	//ros::WallDuration sleep_time(0.01);
	//sleep_time.sleep();

	return collision_result.collision;
}

void MoveKuka::computeIKState(robot_state::RobotState& ik_state,
		const Eigen::Affine3d& end_effector_state)
{

	ros::WallDuration sleep_time(0.01);

	// compute waypoint ik solutions

	const robot_state::JointModelGroup* joint_model_group =
			ik_state.getJointModelGroup(group_name_);

	kinematics::KinematicsQueryOptions options;
	options.return_approximate_solution = false;
	bool found_ik = false;

	robot_state::RobotState org_start(ik_state);
	int i = 0;
	while (true)
	{
		found_ik = ik_state.setFromIK(joint_model_group, end_effector_state, 10,
				0.1, moveit::core::GroupStateValidityCallbackFn(), options);
		ik_state.update();

		found_ik &= !isStateCollide(ik_state);

		if (found_ik && isStateSingular(ik_state))
			found_ik = false;

		double* pos = ik_state.getVariablePositions();
		printf("IK result :");
		for (int i = 0; i < ik_state.getVariableCount(); ++i)
			printf("%f ", pos[i]);
		printf("\n");

		if (found_ik)
			break;

		displayState(ik_state);
		sleep_time.sleep();

		++i;

		double dist = log(-3 + 0.001 * i) / log(10);

		ik_state.setToRandomPositionsNearBy(joint_model_group, org_start, dist);
	}

	if (found_ik)
	{
		ROS_INFO("IK solution found after %d trials", i + 1);
	}
	else
	{
		ROS_INFO("Could not find IK solution");
	}
}

void MoveKuka::printTrajectory(const moveit_msgs::RobotTrajectory &traj)
{
	int num_joints = traj.joint_trajectory.points[0].positions.size();
	for (int k = 0; k < num_joints; ++k)
	{
		std::cout << traj.joint_trajectory.joint_names[k] << " ";
	}
	std::cout << std::endl;

	int num_points = traj.joint_trajectory.points.size();
	for (int j = 0; j < num_points; ++j)
	{
		std::cout << "[" << j << "] ";
		for (int k = 0; k < num_joints; ++k)
		{
			double value = traj.joint_trajectory.points[j].positions[k];
			std::cout << value << " ";
		}
		std::cout << std::endl;
	}
	std::cout << std::endl;

}

void MoveKuka::drawEndeffectorPosition(int id, const Eigen::Vector3d& position)
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

void MoveKuka::drawPath(int id, const Eigen::Vector3d& from,
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

void MoveKuka::renderPRMGraph(robot_state::RobotState& state)
{
	const double trajectory_color_diff = 0.33;
	const double scale = 0.005, scale2 = 0.001;
	;
	const int marker_step = 1;

	visualization_msgs::MarkerArray ma;
	visualization_msgs::Marker::_color_type BLUE, GREEN, LIGHT_YELLOW;
	BLUE.a = 1.0;
	BLUE.r = 1.0;
	BLUE.g = 1.0;
	BLUE.b = 1.0;
	LIGHT_YELLOW = BLUE;
	LIGHT_YELLOW.b = 0.5;
	GREEN.a = 0.1;
	GREEN.r = 0.5;
	GREEN.b = 0.5;
	GREEN.g = 1.0;

	visualization_msgs::Marker msg;
	msg.header.frame_id = robot_model_->getModelFrame();
	msg.header.stamp = ros::Time::now();
	msg.ns = "prm_vertices";
	msg.type = visualization_msgs::Marker::SPHERE_LIST;
	msg.action = visualization_msgs::Marker::ADD;

	msg.scale.x = scale;
	msg.scale.y = scale;
	msg.scale.z = scale;

	msg.id = 0;
	msg.color = LIGHT_YELLOW;

	msg.points.resize(0);
	geometry_msgs::Point point;

	std::ifstream ifs("vertex.txt");
	double data[7];

	for (int c = 0; c < 2000; ++c)
	{
		for (int i = 0; i < 7; ++i)
			ifs >> data[i];
		state.setVariablePositions(data);
		state.updateLinkTransforms();
		const Eigen::Affine3d& transform = state.getGlobalLinkTransform(
				"tcp_1_link");

		point.x = transform.translation()(0);
		point.y = transform.translation()(1);
		point.z = transform.translation()(2);
		msg.points.push_back(point);
	}

	ifs.close();
	ma.markers.push_back(msg);

	msg.id = 1;
	msg.points.resize(0);
	msg.type = visualization_msgs::Marker::LINE_LIST;
	msg.scale.x = scale2;
	msg.scale.y = scale2;
	msg.scale.z = scale2;
	msg.color = GREEN;

	ifs.open("edge.txt");
	for (int c = 0; c < 23746; ++c)
	{
		for (int i = 0; i < 7; ++i)
			ifs >> data[i];
		state.setVariablePositions(data);
		state.updateLinkTransforms();
		const Eigen::Affine3d& transform = state.getGlobalLinkTransform(
				"tcp_1_link");

		point.x = transform.translation()(0);
		point.y = transform.translation()(1);
		point.z = transform.translation()(2);
		msg.points.push_back(point);
	}

	ifs.close();
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

	move_kuka::MoveKuka* move_kuka = new move_kuka::MoveKuka(node_handle);
	//move_itomp->run("lower_body_tcp2");
	move_kuka->run("lower_body");
	delete move_kuka;

	return 0;
}