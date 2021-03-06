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
#include <itomp_ca_planner/common.h>
#include <itomp_ca_planner/trajectory/itomp_cio_trajectory.h>
#include <itomp_ca_planner/model/itomp_robot_model.h>
#include <itomp_ca_planner/model/itomp_planning_group.h>
#include <itomp_ca_planner/util/planning_parameters.h>
#include <ros/console.h>
#include <ros/assert.h>
#include <ecl/geometry/polynomial.hpp>
#include <ecl/geometry.hpp>

using namespace std;

namespace itomp_ca_planner
{

inline int safeToInt(double a)
{
	return (int) (a + 1E-7);
}

ItompCIOTrajectory::ItompCIOTrajectory(const ItompRobotModel* robot_model,
		double duration, double discretization, double num_contacts,
		double contact_phase_duration) :
		robot_model_(robot_model), planning_group_(NULL), num_points_(
				safeToInt(duration / discretization) + 1), num_joints_(
				robot_model_->getNumKDLJoints()), discretization_(
				discretization), duration_(duration), num_contacts_(
				num_contacts), start_index_(1), end_index_(num_points_ - 2), contact_phase_duration_(
				contact_phase_duration), num_contact_phases_(
				safeToInt(duration / contact_phase_duration) + 2), phase_stride_(
				safeToInt(contact_phase_duration / discretization))
{
	ROS_ASSERT(duration == duration_);
	init();
}

ItompCIOTrajectory::ItompCIOTrajectory(const ItompCIOTrajectory& source_traj,
		const ItompPlanningGroup* planning_group, int diff_rule_length) :
		robot_model_(source_traj.robot_model_), planning_group_(planning_group), phase_stride_(
				source_traj.phase_stride_), discretization_(
				source_traj.discretization_)
{
	// TODO: only contacts in this group?
	num_contacts_ = source_traj.num_contacts_;
	contact_phase_duration_ = source_traj.contact_phase_duration_;
	num_contact_phases_ = source_traj.num_contact_phases_;

	num_joints_ = planning_group_->num_joints_;

	int start_extra = (diff_rule_length - 1) - source_traj.start_index_;
	int end_extra = (diff_rule_length - 1)
			- ((source_traj.num_points_ - 1) - source_traj.end_index_);

	num_points_ = source_traj.num_points_ + start_extra + end_extra;

	start_index_ = diff_rule_length - 1;
	end_index_ = (num_points_ - 1) - (diff_rule_length - 1);

	duration_ = source_traj.duration_;

	// allocate the memory:
	init();

	// now copy the trajectories over:
	copyFromFullTrajectory(source_traj);

	contact_trajectory_ = source_traj.contact_trajectory_;

	full_trajectory_index_.resize(num_points_);
	// now copy the trajectories over:
	for (int i = 0; i < num_points_; i++)
	{
		int source_traj_point = i - start_extra;
		if (source_traj_point < 0)
			source_traj_point = 0;
		if (source_traj_point >= source_traj.num_points_)
			source_traj_point = source_traj.num_points_ - 1;
		full_trajectory_index_[i] = source_traj_point;
	}
}

ItompCIOTrajectory::~ItompCIOTrajectory()
{
}

void ItompCIOTrajectory::copyFromFullTrajectory(
		const ItompCIOTrajectory& full_trajectory)
{
	/*
	 for (int i = 0; i < num_joints_; i++)
	 {
	 int source_joint = planning_group_->group_joints_[i].kdl_joint_index_;
	 trajectory_.block(0, i, num_points_, 1) = full_trajectory.trajectory_.block(0, source_joint, num_points_, 1);
	 free_trajectory_.block(0, i, num_contact_phases_ + 1, 1) = full_trajectory.free_trajectory_.block(0, source_joint,
	 num_contact_phases_ + 1, 1);
	 free_vel_trajectory_.block(0, i, num_contact_phases_ + 1, 1) = full_trajectory.free_vel_trajectory_.block(0,
	 source_joint, num_contact_phases_ + 1, 1);
	 }
	 */

	// now copy the trajectories over:
	int start_extra = (DIFF_RULE_LENGTH - 1) - full_trajectory.start_index_;
	for (int i = 0; i < num_points_; i++)
	{
		int source_traj_point = i - start_extra;
		if (source_traj_point < 0)
			source_traj_point = 0;
		if (source_traj_point >= full_trajectory.num_points_)
			source_traj_point = full_trajectory.num_points_ - 1;
		for (int j = 0; j < num_joints_; j++)
		{
			int source_joint =
					planning_group_->group_joints_[j].kdl_joint_index_;
			(*this)(i, j) = full_trajectory(source_traj_point, source_joint);
		}
	}

	// set pre start points using vel, acc
	for (int j = 0; j < num_joints_; j++)
	{
		int source_joint = planning_group_->group_joints_[j].kdl_joint_index_;
		double pos = full_trajectory(0, source_joint);
		double vel = full_trajectory.vel_start_(source_joint);
		double acc = full_trajectory.acc_start_(source_joint);

		// only for root pos
		if (source_joint < 6)
		{
			for (int i = start_extra - 1; i >= 0; --i)
			{
				double new_vel = vel - acc * discretization_;
				double new_pos = pos - vel * discretization_;
				(*this)(i, j) = new_pos;
				vel = new_vel;
				pos = new_pos;
			}
		}
	}
}

void ItompCIOTrajectory::init()
{
	trajectory_ = Eigen::MatrixXd(num_points_, num_joints_);
	contact_trajectory_ = Eigen::MatrixXd(num_contact_phases_ + 1,
			num_contacts_);

	free_trajectory_ = Eigen::MatrixXd(num_contact_phases_ + 1, num_joints_);
	free_vel_trajectory_ = Eigen::MatrixXd::Zero(num_contact_phases_ + 1,
			num_joints_);

	contact_start_points_.clear();
	contact_start_points_.push_back(0);

	for (int i = start_index_; i < end_index_ + 1; i += phase_stride_)
	{
		contact_start_points_.push_back(i);
	}
	contact_start_points_.push_back(end_index_ + 1);
	ROS_ASSERT(contact_start_points_.size() == num_contact_phases_);
}

void ItompCIOTrajectory::updateFromGroupTrajectory(
		const ItompCIOTrajectory& group_trajectory)
{
	/*
	 for (int i = 0; i < group_trajectory.planning_group_->num_joints_; i++)
	 {
	 int target_joint = group_trajectory.planning_group_->group_joints_[i].kdl_joint_index_;
	 trajectory_.block(0, target_joint, num_points_, 1) = group_trajectory.trajectory_.block(0, i, num_points_, 1);
	 free_trajectory_.block(0, target_joint, num_contact_phases_ + 1, 1) = group_trajectory.free_trajectory_.block(0, i,
	 num_contact_phases_ + 1, 1);
	 free_vel_trajectory_.block(0, target_joint, num_contact_phases_ + 1, 1) =
	 group_trajectory.free_vel_trajectory_.block(0, i, num_contact_phases_ + 1, 1);
	 }

	 contact_trajectory_ = group_trajectory.contact_trajectory_;
	 */

	int num_vars_free = end_index_ - start_index_ + 1;
	for (int i = 0; i < group_trajectory.planning_group_->num_joints_; i++)
	{
		int target_joint =
				group_trajectory.planning_group_->group_joints_[i].kdl_joint_index_;
		trajectory_.block(start_index_, target_joint, num_vars_free, 1) =
				group_trajectory.trajectory_.block(
						group_trajectory.start_index_, i, num_vars_free, 1);
	}

	//contact_trajectory_ = group_trajectory.contact_trajectory_;
	int contact_end_index = num_contact_phases_ - 2;
	int contact_start_index = 1;
	int num_contact_vars_free = contact_end_index - contact_start_index + 1;
	for (int i = 0; i < group_trajectory.planning_group_->getNumContacts(); i++)
	{
		// TODO: need to be changed when multiple groups have contacts;
		int target_contact = i;
		contact_trajectory_.block(contact_start_index, target_contact,
				num_contact_vars_free, 1) =
				group_trajectory.contact_trajectory_.block(contact_start_index,
						i, num_contact_vars_free, 1);
	}
}

void ItompCIOTrajectory::updateFromGroupTrajectory(
		const ItompCIOTrajectory& group_trajectory, int point_index,
		int joint_index)
{
	int i = joint_index;
	/*
	 {
	 int target_joint = group_trajectory.planning_group_->group_joints_[i].kdl_joint_index_;
	 trajectory_.block((point_index - 1) * phase_stride_, target_joint, 2 * phase_stride_, 1) =
	 group_trajectory.trajectory_.block((point_index - 1) * phase_stride_, i, 2 * phase_stride_, 1);
	 }
	 */
	int target_joint =
			group_trajectory.planning_group_->group_joints_[i].kdl_joint_index_;
	trajectory_.block(start_index_ + point_index, target_joint, 1, 1) =
			group_trajectory.trajectory_.block(
					group_trajectory.start_index_ + point_index, i, 1, 1);
}

void ItompCIOTrajectory::updateFreePointsFromTrajectory()
{
	/*
	 for (int i = 0; i < num_joints_; ++i)
	 {
	 for (int j = 0; j <= num_contact_phases_; ++j)
	 {
	 free_trajectory_(j, i) = trajectory_(j * phase_stride_, i);
	 }
	 }
	 */

}

void ItompCIOTrajectory::updateTrajectoryFromFreePoints()
{
	/*
	 double coeff[4];
	 double t[4];

	 for (int i = 0; i < num_joints_; ++i)
	 {
	 int pos = 0;
	 for (int j = 0; j < num_contact_phases_; ++j)
	 {
	 double start_pos = free_trajectory_(j, i);
	 double end_pos = free_trajectory_(j + 1, i);
	 double start_vel = free_vel_trajectory_(j, i);
	 double end_vel = free_vel_trajectory_(j + 1, i);

	 coeff[0] = start_pos;
	 coeff[1] = start_vel;
	 coeff[2] = -end_vel + 3 * end_pos - 2 * start_vel - 3 * start_pos;
	 coeff[3] = end_vel - 2 * end_pos + start_vel + 2 * start_pos;

	 for (int k = 0; k < phase_stride_; ++k)
	 {
	 t[0] = 1.0;
	 t[1] = ((double) k) / phase_stride_;
	 t[2] = t[1] * t[1];
	 t[3] = t[2] * t[1];
	 (*this)(pos, i) = 0.0;
	 for (int l = 0; l <= 3; l++)
	 {
	 (*this)(pos, i) += t[l] * coeff[l];
	 }
	 ++pos;
	 }
	 }
	 }
	 */
}

void ItompCIOTrajectory::updateTrajectoryFromFreePoint(int point_index,
		int joint_index)
{
	/*
	 double coeff[4];
	 double t[4];

	 int i = joint_index;
	 {
	 for (int j = point_index - 1; j < point_index + 1; ++j)
	 {
	 ROS_ASSERT(j >= 0 && j < num_contact_phases_);
	 double start_pos = free_trajectory_(j, i);
	 double end_pos = free_trajectory_(j + 1, i);
	 double start_vel = free_vel_trajectory_(j, i);
	 double end_vel = free_vel_trajectory_(j + 1, i);

	 coeff[0] = start_pos;
	 coeff[1] = start_vel;
	 coeff[2] = -end_vel + 3 * end_pos - 2 * start_vel - 3 * start_pos;
	 coeff[3] = end_vel - 2 * end_pos + start_vel + 2 * start_pos;

	 int pos = j * phase_stride_;
	 for (int k = 0; k < phase_stride_; ++k)
	 {
	 t[0] = 1.0;
	 t[1] = ((double) k) / phase_stride_;
	 t[2] = t[1] * t[1];
	 t[3] = t[2] * t[1];
	 (*this)(pos, i) = 0.0;
	 for (int l = 0; l <= 3; l++)
	 {
	 (*this)(pos, i) += t[l] * coeff[l];
	 }
	 ++pos;
	 }
	 }
	 }
	 */
}

void ItompCIOTrajectory::fillInMinJerk(
		const std::set<int>& groupJointsKDLIndices,
		const Eigen::MatrixXd::RowXpr joint_vel_array,
		const Eigen::MatrixXd::RowXpr joint_acc_array)
{
	ROS_INFO("Trajectory 0 use fillInMinJerk");

	vel_start_ = joint_vel_array;
	acc_start_ = joint_acc_array;
	double start_index = start_index_ - 1;
	double end_index = end_index_ + 1;
	double duration = (end_index - start_index) * discretization_;

	double T[6]; // powers of the time duration
	T[0] = 1.0;
	T[1] = duration;

	for (int i = 2; i <= 5; i++)
		T[i] = T[i - 1] * T[1];

	// calculate the spline coefficients for each joint:
	// (these are for the special case of zero start and end vel and acc)
	double coeff[num_joints_][6];

	const int ROT_JOINT_INDEX = 5;

	bool hasRotation = false;
	for (std::set<int>::const_iterator it = groupJointsKDLIndices.begin();
			it != groupJointsKDLIndices.end(); ++it)
	{
		int i = *it;

		// rotation is handled in a special manner

		if (i == ROT_JOINT_INDEX
				&& PlanningParameters::getInstance()->getHasRoot6d())
		{
			if (std::abs((*this)(start_index, 0) - (*this)(end_index, 0)) > 1E-7
					|| std::abs((*this)(start_index, 1) - (*this)(end_index, 1))
							> 1E-7)
			{
				hasRotation = true;
				continue;
			}
		}

		double x0 = (*this)(start_index, i);
		double x1 = (*this)(end_index, i);
		double v0 = (i < 6) ? joint_vel_array(i) : 0.0;
		double a0 = (i < 6) ? joint_acc_array(i) : 0.0;
		ROS_INFO("Joint %d from %f(%f %f) to %f", i, x0, v0, a0, x1);

		v0 = v0 * duration;
		a0 = a0 * duration * duration;

		coeff[i][0] = x0;
		coeff[i][1] = v0;
		coeff[i][2] = 0.5 * a0;
		coeff[i][3] = (-1.5 * a0 - 6 * v0 - 10 * x0 + 10 * x1);
		coeff[i][4] = (1.5 * a0 + 8 * v0 + 15 * x0 - 15 * x1);
		coeff[i][5] = (-0.5 * a0 - 3 * v0 - 6 * x0 + 6 * x1);
	}

	// now fill in the joint positions at each time step
	int numPoints = end_index - start_index;
	for (int i = start_index + 1; i < end_index; i++)
	{
		double t[6]; // powers of the time index point
		t[0] = 1.0;
		t[1] = (double) (i - start_index) / numPoints; //(i - start_index) * discretization_;
		for (int k = 2; k <= 5; k++)
			t[k] = t[k - 1] * t[1];

		for (std::set<int>::const_iterator it = groupJointsKDLIndices.begin();
				it != groupJointsKDLIndices.end(); ++it)
		{
			int j = *it;

			(*this)(i, j) = 0.0;
			for (int k = 0; k <= 5; k++)
			{
				(*this)(i, j) += t[k] * coeff[j][k];
			}
		}
	}

	// rotation reaches the goal in the first contact phase
	if (hasRotation)
	{
		double diff_x = (*this)(end_index, 0) - (*this)(start_index, 0);
		double diff_y = (*this)(end_index, 1) - (*this)(start_index, 1);
		double dir_angle = atan2(diff_y, diff_x) - M_PI * 0.5;

		double interp_indices[] =
		{ start_index, contact_start_points_[2] - 1,
				contact_start_points_[contact_start_points_.size() - 3] - 1,
				contact_start_points_[contact_start_points_.size() - 2] - 1,
				end_index - 1 };
		double interp_values[] =
				{ (*this)(start_index, ROT_JOINT_INDEX), dir_angle, dir_angle,
						(*this)(end_index, ROT_JOINT_INDEX), (*this)(end_index,
								ROT_JOINT_INDEX) };

		for (int idx = 0; idx < 4; ++idx)
		{
			double interp_start_index = interp_indices[idx];
			double interp_end_index = interp_indices[idx + 1];
			double duration = (interp_end_index - interp_start_index)
					* discretization_;

			double T[6]; // powers of the time duration
			T[0] = 1.0;
			T[1] = duration;

			for (int i = 2; i <= 5; i++)
				T[i] = T[i - 1] * T[1];

			// calculate the spline coefficients for each joint:
			// (these are for the special case of zero start and end vel and acc)
			double coeff[num_joints_][6];
			{
				double x0 = interp_values[idx];
				double x1 = interp_values[idx + 1];
				double v0 = (idx == 0) ? joint_vel_array(ROT_JOINT_INDEX) : 0.0;
				double a0 = (idx == 0) ? joint_acc_array(ROT_JOINT_INDEX) : 0.0;
				//ROS_INFO("Joint %d from %f(%f %f) to %f", i, x0, v0, a0, x1);

				v0 = v0 * duration;
				a0 = a0 * duration * duration;

				coeff[ROT_JOINT_INDEX][0] = x0;
				coeff[ROT_JOINT_INDEX][1] = v0;
				coeff[ROT_JOINT_INDEX][2] = 0.5 * a0;
				coeff[ROT_JOINT_INDEX][3] = (-1.5 * a0 - 6 * v0 - 10 * x0
						+ 10 * x1);
				coeff[ROT_JOINT_INDEX][4] = (1.5 * a0 + 8 * v0 + 15 * x0
						- 15 * x1);
				coeff[ROT_JOINT_INDEX][5] = (-0.5 * a0 - 3 * v0 - 6 * x0
						+ 6 * x1);
			}

			// now fill in the joint positions at each time step
			int numPoints = interp_end_index - interp_start_index;
			for (int i = interp_start_index + 1; i <= interp_end_index; i++)
			{
				double t[6]; // powers of the time index point
				t[0] = 1.0;
				t[1] = (double) (i - interp_start_index) / numPoints;
				for (int k = 2; k <= 5; k++)
					t[k] = t[k - 1] * t[1];

				{
					int j = ROT_JOINT_INDEX;

					(*this)(i, j) = 0.0;
					for (int k = 0; k <= 5; k++)
					{
						(*this)(i, j) += t[k] * coeff[j][k];
					}
				}
			}
		}
	}
}

void ItompCIOTrajectory::fillInMinJerk(int trajectory_index,
		const std::set<int>& groupJointsKDLIndices,
		const ItompPlanningGroup* planning_group,
		const moveit_msgs::TrajectoryConstraints& trajectory_constraints,
		const Eigen::MatrixXd::RowXpr joint_vel_array,
		const Eigen::MatrixXd::RowXpr joint_acc_array)
{
	vel_start_ = joint_vel_array;
	acc_start_ = joint_acc_array;

	//printTrajectory();
	int num_points = getNumPoints();

	std::string trajectory_index_string = boost::lexical_cast<std::string>(
			trajectory_index);
	int traj_constraint_begin = 0;
	int traj_constraint_end = trajectory_constraints.constraints.size();
	int i = 0;
	for (i = 0; i < trajectory_constraints.constraints.size(); ++i)
	{
		if (trajectory_constraints.constraints[i].name
				== trajectory_index_string)
		{
			traj_constraint_begin = i;
			break;
		}
	}
	for (; i < trajectory_constraints.constraints.size(); ++i)
	{
		if (trajectory_constraints.constraints[i].name == "end")
		{
			traj_constraint_end = i + 1;
			break;
		}
	}

	// temporary change
	int num_constraint_points = traj_constraint_end - traj_constraint_begin;

	std::vector<double> acc_dist(num_constraint_points);
	acc_dist[0] = 0.0;
	for (int i = 1; i < num_constraint_points; ++i)
	{
		double d = 0.0;
		for (int k = 0;
				k < trajectory_constraints.constraints[traj_constraint_begin].joint_constraints.size(); ++k)
		{
			double x0 =	trajectory_constraints.constraints[i - 1].joint_constraints[k].position;
			double x1 =	trajectory_constraints.constraints[i].joint_constraints[k].position;
			d += (x1 - x0) * (x1 - x0);
		}
		acc_dist[i] = acc_dist[i - 1] + sqrt(d);
	}
	double acc_dist_end = acc_dist[num_constraint_points - 1];

	double interval = (double) num_points / (num_constraint_points - 1);

	int group_joint_index = 0;
	for (std::set<int>::const_iterator it = groupJointsKDLIndices.begin();
			it != groupJointsKDLIndices.end(); ++it)
	{
		int j = *it;

		bool has_constraints = false;
		int constraint_index = -1;
		for (int k = 0;
				k
						< trajectory_constraints.constraints[traj_constraint_begin].joint_constraints.size();
				++k)
		{

			if (trajectory_constraints.constraints[traj_constraint_begin].joint_constraints[k].joint_name
					== planning_group->group_joints_[group_joint_index].joint_name_)
			{

				has_constraints = true;
				constraint_index = k;
			}
		}

		if (!has_constraints)
		{
			double x0 = (*this)(0, j);
			double v0 = 0.0;
			double a0 = 0.0;

			double x1 = (*this)(getNumPoints(), j);
			double v1 = 0.0;
			double a1 = 0.0;

			ecl::QuinticPolynomial poly;
			poly = ecl::QuinticPolynomial::Interpolation(0, x0, v0, a0,
					duration_, x1, v1, a1);
			for (int i = 1; i < getNumPoints() - 1; ++i)
			{
				(*this)(i, j) = poly(i * discretization_);
			}
		}
		else
		{
			double x0 =
					trajectory_constraints.constraints[traj_constraint_begin].joint_constraints[constraint_index].position;
			double v0 = 0.0;
			double a0 = 0.0;

			double x1 = trajectory_constraints.constraints[traj_constraint_end
					- 1].joint_constraints[constraint_index].position;
			;
			double v1 = 0.0;
			double a1 = 0.0;

			ecl::CubicSpline cubic;
			ecl::Array<double> array_x(num_constraint_points);
			ecl::Array<double> array_y(num_constraint_points);
			for (int k = 0; k < num_constraint_points; ++k)
			{
				int point = k + traj_constraint_begin;

				array_x[k] = acc_dist[k];
				array_y[k] =
						trajectory_constraints.constraints[point].joint_constraints[constraint_index].position;
				
			}
			cubic = ecl::CubicSpline::Natural(array_x, array_y);
			for (int i = 0; i < getNumPoints() - 1; ++i)
			{
				double x = (double)i / (getNumPoints() - 1) * acc_dist_end;
				double value = cubic(x);
				(*this)(i, j) = value;
			}
		}
		++group_joint_index;
	}
	//printTrajectory();
}

void ItompCIOTrajectory::fillInMinJerkCartesianTrajectory(
		const std::set<int>& groupJointsKDLIndices,
		const Eigen::MatrixXd::RowXpr joint_vel_array,
		const Eigen::MatrixXd::RowXpr joint_acc_array,
		const moveit_msgs::Constraints& path_constraints,
		const string& group_name)
{
	robot_state::RobotStatePtr kinematic_state(
			new robot_state::RobotState(robot_model_->getRobotModel()));
	const robot_state::JointModelGroup* joint_model_group =
			robot_model_->getRobotModel()->getJointModelGroup(group_name);

	geometry_msgs::Vector3 start_position =
			path_constraints.position_constraints[0].target_point_offset;
	geometry_msgs::Vector3 goal_position =
			path_constraints.position_constraints[1].target_point_offset;
	geometry_msgs::Quaternion orientation =
			path_constraints.orientation_constraints[0].orientation;

	vel_start_ = joint_vel_array;
	acc_start_ = joint_acc_array;
	double start_index = start_index_ - 1;
	double end_index = end_index_ + 1;
	double duration = (end_index - start_index) * discretization_;

	// set state to the start config
	std::vector<double> positions(num_joints_);
	for (std::size_t k = 0; k < num_joints_; k++)
	{
		positions[k] = (*this)(start_index, k);
	}
	kinematic_state->setVariablePositions(&positions[0]);
	kinematic_state->update();

	double T[6]; // powers of the time duration
	T[0] = 1.0;
	T[1] = duration;

	for (int i = 2; i <= 5; i++)
		T[i] = T[i - 1] * T[1];

	// calculate the spline coefficients for 3d space
	const int CARTESIAN_SPACE_DOF = 3;
	double coeff[CARTESIAN_SPACE_DOF][6];

	for (int i = 0; i < 3; ++i)
	{
		double x0, x1;
		switch (i)
		{
		case 0:
			x0 = start_position.x;
			x1 = goal_position.x;
			break;
		case 1:
			x0 = start_position.y;
			x1 = goal_position.y;
			break;
		case 2:
			x0 = start_position.z;
			x1 = goal_position.z;
			break;
		}

		double v0 = 0.0;
		double a0 = 0.0;
		ROS_INFO("CartesianSpace %d from %f(%f %f) to %f", i, x0, v0, a0, x1);

		v0 = v0 * duration;
		a0 = a0 * duration * duration;

		coeff[i][0] = x0;
		coeff[i][1] = v0;
		coeff[i][2] = 0.5 * a0;
		coeff[i][3] = (-1.5 * a0 - 6 * v0 - 10 * x0 + 10 * x1);
		coeff[i][4] = (1.5 * a0 + 8 * v0 + 15 * x0 - 15 * x1);
		coeff[i][5] = (-0.5 * a0 - 3 * v0 - 6 * x0 + 6 * x1);
	}

	// now evaluate 3d pos for each pos
	int numPoints = end_index - start_index;
	for (int i = start_index; i <= end_index; i++)
	{
		double t[6]; // powers of the time index point
		t[0] = 1.0;
		t[1] = (double) (i - start_index) / numPoints; //(i - start_index) * discretization_;
		for (int k = 2; k <= 5; k++)
			t[k] = t[k - 1] * t[1];

		double pos[3];

		for (int j = 0; j < 3; ++j)
		{
			pos[j] = 0;
			for (int k = 0; k <= 5; k++)
			{
				pos[j] += t[k] * coeff[j][k];
			}
		}
		geometry_msgs::Vector3 position;
		position.x = pos[0];
		position.y = pos[1];
		position.z = pos[2];

		// Use IK to compute joint values
		vector<double> ik_solution(num_joints_);

		Eigen::Affine3d end_effector_state = Eigen::Affine3d::Identity();
		Eigen::Quaternion<double> rot(orientation.w, orientation.x,
				orientation.y, orientation.z);
		Eigen::Vector3d trans(position.x, position.y, position.z);
		Eigen::Matrix3d mat = rot.toRotationMatrix();
		end_effector_state.linear() = mat;
		end_effector_state.translation() = trans;

		kinematics::KinematicsQueryOptions options;
		options.return_approximate_solution = true;
		bool found_ik = false;
		while (found_ik == false)
		{
			found_ik = kinematic_state->setFromIK(joint_model_group,
					end_effector_state, 10, 0.1,
					moveit::core::GroupStateValidityCallbackFn(), options);
			if (found_ik)
			{
				//ROS_INFO("IK solution found for waypoint %d", i);

				std::vector<double> group_values;
				kinematic_state->copyJointGroupPositions(joint_model_group,
						group_values);
				double* state_pos = kinematic_state->getVariablePositions();
				ROS_ASSERT(num_joints_ == kinematic_state->getVariableCount());
				//printf("EE : (%f %f %f)(%f %f %f %f) : ", position.x, position.y, position.z, orientation.x, orientation.y,
				//  orientation.z, orientation.w);
				for (std::size_t k = 0; k < kinematic_state->getVariableCount();
						++k)
				{
					ik_solution[k] = state_pos[k];
					if (i != start_index)
						(*this)(i, k) = state_pos[k];
					//printf("%f ", state_pos[k]);
				}
				//printf("\n");
				break;
			}
			else
			{
				ROS_INFO("Could not find IK solution for waypoint %d", i);
			}
		}
	}
}

void ItompCIOTrajectory::printTrajectory() const
{
	printf("Full Trajectory\n");
	for (int i = 0; i < num_points_; ++i)
	{
		printf("%d : ", i);
		for (int j = 0; j < num_joints_; ++j)
		{
			printf("%f ", trajectory_(i, j));
		}
		printf("\n");
	}
}

}
