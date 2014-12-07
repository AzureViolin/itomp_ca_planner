#ifndef ITOMP_PLANNING_INTERFACE_H_
#define ITOMP_PLANNING_INTERFACE_H_

#include <moveit/planning_interface/planning_interface.h>
#include <itomp_ca_planner/planner/itomp_planner_node.h>

namespace itomp_ca_planner
{

class ItompPlanningContext: public planning_interface::PlanningContext
{
public:
	ItompPlanningContext(const std::string &name, const std::string &group);
	virtual ~ItompPlanningContext();

	bool initialize(const robot_model::RobotModelConstPtr& model);

	virtual bool solve(planning_interface::MotionPlanResponse &res);
	virtual bool solve(planning_interface::MotionPlanDetailedResponse &res);

	virtual void clear();
	virtual bool terminate();

	void setPlanRequest(const planning_interface::MotionPlanRequest& req);

private:
	boost::scoped_ptr<ItompPlannerNode> itomp_planner_node_;
	planning_interface::MotionPlanRequest req_;
};

}

#endif
