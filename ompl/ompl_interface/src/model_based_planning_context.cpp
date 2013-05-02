/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2012, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan, Sachin Chitta */

#include <moveit/ompl_interface/model_based_planning_context.h>
#include <moveit/ompl_interface/detail/state_validity_checker.h>
#include <moveit/ompl_interface/detail/constrained_sampler.h>
#include <moveit/ompl_interface/detail/constrained_goal_sampler.h>
#include <moveit/ompl_interface/detail/goal_union.h>
#include <moveit/ompl_interface/detail/projection_evaluators.h>
#include <moveit/ompl_interface/constraints_library.h>
#include <moveit/kinematic_constraints/utils.h>
#include <eigen_conversions/eigen_msg.h>

#include <ompl/base/samplers/UniformValidStateSampler.h>
#include <ompl/base/goals/GoalLazySamples.h>
#include <ompl/tools/config/SelfConfig.h>
#include <ompl/tools/debug/Profiler.h>
#include <ompl/base/spaces/SE3StateSpace.h>
#include <ompl/datastructures/PDF.h>

ompl_interface::ModelBasedPlanningContext::ModelBasedPlanningContext(const std::string &name, const ModelBasedPlanningContextSpecification &spec) :
  spec_(spec), name_(name), complete_initial_robot_state_(spec.state_space_->getRobotModel()),
  ompl_simple_setup_(spec.state_space_), ompl_benchmark_(ompl_simple_setup_), ompl_parallel_plan_(ompl_simple_setup_.getProblemDefinition()),
  last_plan_time_(0.0), last_simplify_time_(0.0), max_goal_samples_(0), max_state_sampling_attempts_(0), max_goal_sampling_attempts_(0), 
  max_planning_threads_(0), max_solution_segment_length_(0.0), minimum_waypoint_count_(0)
{
  ompl_simple_setup_.getStateSpace()->computeSignature(space_signature_);
  ompl_simple_setup_.getStateSpace()->setStateSamplerAllocator(boost::bind(&ModelBasedPlanningContext::allocPathConstrainedSampler, this, _1));
}

void ompl_interface::ModelBasedPlanningContext::setProjectionEvaluator(const std::string &peval)
{
  if (!spec_.state_space_)
  {
    logError("No state space is configured yet");
    return;
  }
  ob::ProjectionEvaluatorPtr pe = getProjectionEvaluator(peval);
  if (pe)
    spec_.state_space_->registerDefaultProjection(pe);
}

ompl::base::ProjectionEvaluatorPtr ompl_interface::ModelBasedPlanningContext::getProjectionEvaluator(const std::string &peval) const
{
  if (peval.find_first_of("link(") == 0 && peval[peval.length() - 1] == ')')
  {
    std::string link_name = peval.substr(5, peval.length() - 6);
    if (getRobotModel()->hasLinkModel(link_name))
      return ob::ProjectionEvaluatorPtr(new ProjectionEvaluatorLinkPose(this, link_name));
    else
      logError("Attempted to set projection evaluator with respect to position of link '%s', but that link is not known to the kinematic model.", link_name.c_str());
  }
  else
    if (peval.find_first_of("joints(") == 0 && peval[peval.length() - 1] == ')')
    {
      std::string joints = peval.substr(7, peval.length() - 8);
      boost::replace_all(joints, ",", " ");
      std::vector<std::pair<std::string, unsigned int> > j;
      std::stringstream ss(joints);
      while (ss.good() && !ss.eof())
      {
	std::string v; ss >> v >> std::ws;
	if (getJointModelGroup()->hasJointModel(v))
	{
	  unsigned int vc = getJointModelGroup()->getJointModel(v)->getVariableCount();
	  if (vc > 0)
	    j.push_back(std::make_pair(v, vc));
	  else
	    logWarn("%s: Ignoring joint '%s' in projection since it has 0 DOF", name_.c_str(), v.c_str());
	}
	else
	  logError("%s: Attempted to set projection evaluator with respect to value of joint '%s', but that joint is not known to the group '%s'.",
                   name_.c_str(), v.c_str(), getJointModelGroup()->getName().c_str());
      }
      if (j.empty())
	logError("%s: No valid joints specified for joint projection", name_.c_str());
      else
	return ob::ProjectionEvaluatorPtr(new ProjectionEvaluatorJointValue(this, j));
    }
    else
      logError("Unable to allocate projection evaluator based on description: '%s'", peval.c_str());  
  return ob::ProjectionEvaluatorPtr();
}

ompl::base::StateSamplerPtr ompl_interface::ModelBasedPlanningContext::allocPathConstrainedSampler(const ompl::base::StateSpace *ss) const
{
  if (spec_.state_space_.get() != ss)
  {
    logError("%s: Attempted to allocate a state sampler for an unknown state space", name_.c_str());
    return ompl::base::StateSamplerPtr();
  }
  
  logDebug("%s: Allocating a new state sampler (attempts to use path constraints)", name_.c_str());
  
  if (path_constraints_)
  {
    if (spec_.constraints_library_)
    {
      const ConstraintApproximationPtr &ca = spec_.constraints_library_->getConstraintApproximation(path_constraints_msg_);
      if (ca)
      {
        ompl::base::StateSamplerAllocator c_ssa = ca->getStateSamplerAllocator(path_constraints_msg_);
        if (c_ssa)
        {
          ompl::base::StateSamplerPtr res = c_ssa(ss);
          if (res)
          {
            logDebug("Using precomputed state sampler (approximated constraint space)");
            return res;
          }
        }
      }
    }  

    constraint_samplers::ConstraintSamplerPtr cs;
    if (spec_.constraint_sampler_manager_)
      cs = spec_.constraint_sampler_manager_->selectSampler(getPlanningScene(), getJointModelGroup()->getName(), path_constraints_->getAllConstraints());
    
    if (cs)
    {
      logDebug("%s: Allocating specialized state sampler for state space", name_.c_str());
      return ob::StateSamplerPtr(new ConstrainedSampler(this, cs));
    }
  }
  logDebug("%s: Allocating default state sampler for state space", name_.c_str());
  return ss->allocDefaultStateSampler();
}

void ompl_interface::ModelBasedPlanningContext::configure()
{
  // convert the input state to the corresponding OMPL state
  ompl::base::ScopedState<> ompl_start_state(spec_.state_space_);
  spec_.state_space_->copyToOMPLState(ompl_start_state.get(), getCompleteInitialRobotState());
  ompl_simple_setup_.setStartState(ompl_start_state);
  ompl_simple_setup_.setStateValidityChecker(ob::StateValidityCheckerPtr(new StateValidityChecker(this)));
    
  useConfig();  
  if (ompl_simple_setup_.getGoal() && follow_samplers_.empty())
    ompl_simple_setup_.setup();
}

void ompl_interface::ModelBasedPlanningContext::useConfig()
{
  const std::map<std::string, std::string> &config = spec_.config_;
  if (config.empty())
    return;
  std::map<std::string, std::string> cfg = config;
  
  // set the projection evaluator
  std::map<std::string, std::string>::iterator it = cfg.find("projection_evaluator");
  if (it != cfg.end())
  {
    setProjectionEvaluator(boost::trim_copy(it->second));
    cfg.erase(it);
  }
  
  if (cfg.empty())
    return;
  
  it = cfg.find("type");
  if (it == cfg.end())
  {
    if (name_ != getJointModelGroupName())
      logWarn("%s: Attribute 'type' not specified in planner configuration", name_.c_str());
  }
  else
  {
    // remove the 'type' parameter; the rest are parameters for the planner itself
    std::string type = it->second;
    cfg.erase(it);
    ompl_simple_setup_.setPlannerAllocator(boost::bind(spec_.planner_selector_(type), _1,
						       name_ != getJointModelGroupName() ? name_ : "", spec_));
    logInform("Planner configuration '%s' will use planner '%s'. Additional configuration parameters will be set when the planner is constructed.",
              name_.c_str(), type.c_str());
  }
  
  // call the setParams() after setup(), so we know what the params are
  ompl_simple_setup_.getSpaceInformation()->setup();
  ompl_simple_setup_.getSpaceInformation()->params().setParams(cfg, true);
  // call setup() again for possibly new param values
  ompl_simple_setup_.getSpaceInformation()->setup();
}

void ompl_interface::ModelBasedPlanningContext::setPlanningVolume(const moveit_msgs::WorkspaceParameters &wparams)
{
  if (wparams.min_corner.x == wparams.max_corner.x && wparams.min_corner.x == 0.0 &&
      wparams.min_corner.y == wparams.max_corner.y && wparams.min_corner.y == 0.0 &&
      wparams.min_corner.z == wparams.max_corner.z && wparams.min_corner.z == 0.0)
    logWarn("It looks like the planning volume was not specified.");
  
  logDebug("%s: Setting planning volume (affects SE2 & SE3 joints only) to x = [%f, %f], y = [%f, %f], z = [%f, %f]", name_.c_str(),
           wparams.min_corner.x, wparams.max_corner.x, wparams.min_corner.y, wparams.max_corner.y, wparams.min_corner.z, wparams.max_corner.z);
  
  spec_.state_space_->setPlanningVolume(wparams.min_corner.x, wparams.max_corner.x,
                                        wparams.min_corner.y, wparams.max_corner.y,
                                        wparams.min_corner.z, wparams.max_corner.z);
}

void ompl_interface::ModelBasedPlanningContext::simplifySolution(double timeout)
{
  ompl_simple_setup_.simplifySolution(timeout);
  last_simplify_time_ = ompl_simple_setup_.getLastSimplificationTime();
}

void ompl_interface::ModelBasedPlanningContext::interpolateSolution()
{
  if (ompl_simple_setup_.haveSolutionPath())
  {
    og::PathGeometric &pg = ompl_simple_setup_.getSolutionPath();
    pg.interpolate(std::max((unsigned int)floor(0.5 + pg.length() / max_solution_segment_length_), minimum_waypoint_count_));
  }
}

void ompl_interface::ModelBasedPlanningContext::convertPath(const ompl::geometric::PathGeometric &pg, robot_trajectory::RobotTrajectory &traj) const
{
  robot_state::RobotState ks = complete_initial_robot_state_;
  for (std::size_t i = 0 ; i < pg.getStateCount() ; ++i)
  {
    spec_.state_space_->copyToRobotState(ks, pg.getState(i));
    traj.addSuffixWayPoint(ks, 0.0);
  }
}

bool ompl_interface::ModelBasedPlanningContext::getSolutionPath(robot_trajectory::RobotTrajectory &traj) const
{
  traj.clear();
  if (!ompl_simple_setup_.haveSolutionPath())
    return false;
  convertPath(ompl_simple_setup_.getSolutionPath(), traj);
  return true;
}

void ompl_interface::ModelBasedPlanningContext::setVerboseStateValidityChecks(bool flag)
{
  if (ompl_simple_setup_.getStateValidityChecker())
    static_cast<StateValidityChecker*>(ompl_simple_setup_.getStateValidityChecker().get())->setVerbose(flag);
}

ompl::base::GoalPtr ompl_interface::ModelBasedPlanningContext::constructGoal()
{ 
  // ******************* set up the goal representation, based on goal constraints
  
  std::vector<ob::GoalPtr> goals;
  for (std::size_t i = 0 ; i < goal_constraints_.size() ; ++i)
  {
    constraint_samplers::ConstraintSamplerPtr cs;
    if (spec_.constraint_sampler_manager_)
      cs = spec_.constraint_sampler_manager_->selectSampler(getPlanningScene(), getJointModelGroup()->getName(), goal_constraints_[i]->getAllConstraints());
    if (cs)
    {
      ob::GoalPtr g = ob::GoalPtr(new ConstrainedGoalSampler(this, goal_constraints_[i], cs));
      goals.push_back(g);
    }
  }
  
  if (!goals.empty())
    return goals.size() == 1 ? goals[0] : ompl::base::GoalPtr(new GoalSampleableRegionMux(goals));
  else
    logError("Unable to construct goal representation");
  
  return ob::GoalPtr();
}

void ompl_interface::ModelBasedPlanningContext::setPlanningScene(const planning_scene::PlanningSceneConstPtr &planning_scene)
{
  planning_scene_ = planning_scene;
}

void ompl_interface::ModelBasedPlanningContext::setCompleteInitialState(const robot_state::RobotState &complete_initial_robot_state)
{
  complete_initial_robot_state_ = complete_initial_robot_state;
}

void ompl_interface::ModelBasedPlanningContext::clear()
{
  ompl_simple_setup_.clear();
  ompl_simple_setup_.clearStartStates();
  ompl_simple_setup_.setGoal(ob::GoalPtr());  
  ompl_simple_setup_.setStateValidityChecker(ob::StateValidityCheckerPtr());
  path_constraints_.reset();
  goal_constraints_.clear();
}

bool ompl_interface::ModelBasedPlanningContext::setPathConstraints(const moveit_msgs::Constraints &path_constraints,
								   moveit_msgs::MoveItErrorCodes *error)
{
  // ******************* set the path constraints to use
  path_constraints_.reset(new kinematic_constraints::KinematicConstraintSet(getPlanningScene()->getRobotModel(), getPlanningScene()->getTransforms()));
  path_constraints_->add(path_constraints);
  path_constraints_msg_ = path_constraints;
  
  return true;
}
								   
bool ompl_interface::ModelBasedPlanningContext::setGoalConstraints(const std::vector<moveit_msgs::Constraints> &goal_constraints,
								   const moveit_msgs::Constraints &path_constraints,
								   moveit_msgs::MoveItErrorCodes *error)
{
  
  // ******************* check if the input is correct
  goal_constraints_.clear();
  for (std::size_t i = 0 ; i < goal_constraints.size() ; ++i)
  {
    moveit_msgs::Constraints constr = kinematic_constraints::mergeConstraints(goal_constraints[i], path_constraints);
    kinematic_constraints::KinematicConstraintSetPtr kset(new kinematic_constraints::KinematicConstraintSet(getPlanningScene()->getRobotModel(), getPlanningScene()->getTransforms()));
    kset->add(constr);
    if (!kset->empty())
      goal_constraints_.push_back(kset);
  }

  if (goal_constraints_.empty())
  {
    logWarn("%s: No goal constraints specified. There is no problem to solve.", name_.c_str());
    if (error)
      error->val = moveit_msgs::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS;
    return false;
  }

  ob::GoalPtr goal = constructGoal();
  ompl_simple_setup_.setGoal(goal);
  if (goal)
    return true;
  else
    return false;
}

bool ompl_interface::ModelBasedPlanningContext::benchmark(double timeout, unsigned int count, const std::string &filename)
{
  ompl_benchmark_.clearPlanners();
  ompl_simple_setup_.setup();  
  ompl_benchmark_.addPlanner(ompl_simple_setup_.getPlanner());
  ompl_benchmark_.setExperimentName(getRobotModel()->getName() + "_" + getJointModelGroupName() + "_" +
				    getPlanningScene()->getName() + "_" + name_);
  
  ot::Benchmark::Request req;
  req.maxTime = timeout;
  req.runCount = count;
  req.displayProgress = true;
  req.saveConsoleOutput = false;
  ompl_benchmark_.benchmark(req);
  return filename.empty() ? ompl_benchmark_.saveResultsToFile() : ompl_benchmark_.saveResultsToFile(filename.c_str());
}

/** @cond IGNORE */

namespace ompl
{
namespace geometric
{
using namespace ompl_interface;

class Follower : private boost::noncopyable
{
public:
  
  Follower(const base::SpaceInformationPtr &si) : si_(si), goalBias_(0.05)
  {
  }

  ~Follower()
  {
  }
  
  const base::SpaceInformationPtr &getSpaceInformation() const
  {
    return si_;
  }
  
  const base::ProblemDefinitionPtr& getProblemDefinition() const
  {
    return pdef_;
  }
  
  void setProblemDefinition(const base::ProblemDefinitionPtr &pdef)
  {
    pdef_ = pdef;
    pis_.use(pdef_);
  } 

  base::ParamSet& params()
  {
    return params_;
  }

  const base::ParamSet& params() const
  {
    return params_;
  }

  base::PlannerStatus follow(const std::vector<ValidConstrainedSamplerPtr> &samplers, const base::PlannerTerminationCondition &ptc);
  
private:

  void computeSolution(const std::vector<std::vector<base::State*> > &sets,
                       const std::vector< std::vector< std::vector<std::size_t> > > &connections);

  void propagateStartInfo(std::size_t set_index, std::size_t elem_index,
                          std::vector<std::vector<int> > &is_start,
                          const std::vector< std::vector< std::vector<std::size_t> > > &connections);
  
  bool findSolutionPath(PathGeometric &path, std::size_t set_index, std::size_t elem_index,
                        const std::vector<std::vector<base::State*> > &sets,
                        const std::vector< std::vector< std::vector<std::size_t> > > &connections);
  
  base::SpaceInformationPtr  si_;
  base::ProblemDefinitionPtr pdef_;
  base::PlannerInputStates   pis_;
  base::ParamSet             params_; 
  double                     goalBias_;
  RNG                        rng_;
};

base::PlannerStatus Follower::follow(const std::vector<ValidConstrainedSamplerPtr> &samplers, const base::PlannerTerminationCondition &ptc)
{
  if (!si_->isSetup())
    si_->setup();
  
  pis_.checkValidity();

  if (!pdef_->getGoal()->hasType(base::GOAL_SAMPLEABLE_REGION))
  {
    OMPL_ERROR("The goal region must be sampleable");
    return base::PlannerStatus::UNRECOGNIZED_GOAL_TYPE;
  }
  
  std::vector<std::vector<base::State*> > sets(samplers.size() + 2);
  
  // fill in start states
  while (const base::State *st = pis_.nextStart())
    sets[0].push_back(si_->cloneState(st));
  
  if (sets[0].empty())
  {
    OMPL_ERROR("No valid start states found.");
    return base::PlannerStatus::INVALID_START;
  } 
  
  base::PlannerStatus result = base::PlannerStatus::EXACT_SOLUTION;
  base::GoalSampleableRegion *goal = static_cast<base::GoalSampleableRegion*>(pdef_->getGoal().get());
  
  base::State *work_area = si_->allocState();
  
  // try to generate at least one sample from every sampler
  for (std::size_t i = 0 ; i < samplers.size() && !ptc() ; ++i)
  { 
    while (sets[i + 1].empty() && !ptc())
      if (sets[i].empty())
      {
        if (samplers[i]->sample(work_area) && si_->isValid(work_area))
          sets[i + 1].push_back(si_->cloneState(work_area));
      }
      else
      {
        si_->copyState(work_area, sets[i].back());
        if ((samplers[i]->project(work_area) || samplers[i]->sample(work_area)) && si_->isValid(work_area))
          sets[i + 1].push_back(si_->cloneState(work_area));
      }
  }
  
  if (ptc())
    result = base::PlannerStatus::TIMEOUT;
  else
  {
    // add at least one goal state
    if (const base::State *st = pis_.nextGoal(ptc))
      sets.back().push_back(si_->cloneState(st));
    else
    {
      OMPL_ERROR("Unable to sample any valid states for goal tree");
      result = base::PlannerStatus::INVALID_GOAL;
    }
  }
  if (result == base::PlannerStatus::EXACT_SOLUTION)
  {
    std::vector<std::vector<std::vector<std::size_t> > > connections(sets.size() - 1);
  
    // check connections between first states (heuristic)
    bool first_sample_worked = true;
    for (std::size_t i = 0 ; i < connections.size() ; ++i)
    {
      connections[i].resize(sets[i].size());    
      if (si_->checkMotion(sets[i][0], sets[i+1][0]))
        connections[i][0].push_back(0);
      else
        first_sample_worked = false;
    }

    if (first_sample_worked)
    {
      // we are done; we have a solution
      logDebug("First samples were successfully connected for all sets of constraints. Solution can be reported.");
      computeSolution(sets, connections);
    }
    else
    {
      // create a PDF over the sampled states
      PDF<unsigned int> pdf_sets;
      std::vector<PDF<unsigned int>::Element*> pdf_elements;
      const double weight_offset = 1.0 / (double)sets.size();
      for (std::size_t i = 1 ; i < sets.size() ; ++i)
        pdf_elements.push_back(pdf_sets.add(i, 1.0 / (weight_offset + (double)(sets[i].size()))));
      
      // add further connections from start states (if any)
      for (std::size_t i = 1 ; i < sets[0].size() ; ++i)
        if (si_->checkMotion(sets[0][i], sets[1][0]))
          connections[0][i].push_back(0);

      // remember which states are connected to the start
      std::vector< std::vector<int> > is_start(sets.size());
      is_start[0].resize(sets[0].size(), 1);
      for (std::size_t i = 1 ; i < sets.size() ; ++i)
        is_start[i].resize(sets[i].size(), 0);

      // propagate start info
      for (std::size_t i = 0 ; i < sets[0].size() ; ++i)
        propagateStartInfo(0, i, is_start, connections);

      unsigned int goal_index = sets.size() - 1;
      bool solved = false;int steps = 0;
      
      bool adding_goals = true;
      while (!ptc() && !solved)
      {
        bool added = false;
        unsigned int index = pdf_sets.sample(rng_.uniform01());
        if (index == goal_index || (adding_goals && rng_.uniform01() < goalBias_))
        {
          const base::State *st = pis_.nextGoal();
          if (st)
          {
            sets[goal_index].push_back(si_->cloneState(st));
            is_start[goal_index].push_back(0);
            pdf_sets.update(pdf_elements.back(), 1.0 / (weight_offset + (double)(sets[goal_index].size())));
            added = true;
          }
          else
            adding_goals = false;
        }
        else
        {
          if (samplers[index - 1]->sample(work_area) && si_->isValid(work_area))
          {
            sets[index].push_back(si_->cloneState(work_area));
            connections[index].resize(connections[index].size() + 1);
            is_start[index].push_back(0);
            pdf_sets.update(pdf_elements[index - 1], 1.0 / (weight_offset + (double)(sets[index].size())));
            added = true;
          }
        }
        if (added)
        {          
          const std::vector<base::State*> &prev = sets[index-1];          
          std::size_t added_elem_index = sets[index].size() - 1;
          for (std::size_t i = 0 ; i < prev.size() ; ++i)
            if (si_->checkMotion(prev[i], sets[index].back()))
            {
              connections[index-1][i].push_back(added_elem_index);
              if (is_start[index-1][i] == 1)
              {
                is_start[index][added_elem_index] = 1;
                propagateStartInfo(index, added_elem_index, is_start, connections);
              }
            }
          
          if (index < goal_index)
          {   
            const std::vector<base::State*> &next = sets[index+1];          
            for (std::size_t i = 0 ; i < next.size() ; ++i)
              if (si_->checkMotion(sets[index].back(), next[i]))
              {
                connections[index][added_elem_index].push_back(i);
                if (is_start[index][added_elem_index] == 1 && is_start[index + 1][i] == 0)
                {
                  is_start[index + 1][i] = 1;
                  propagateStartInfo(index + 1, i, is_start, connections);
                }
              }
          }

          for (std::size_t i = 0 ; i < is_start[goal_index].size() ; ++i)
            if (is_start[goal_index][i] == 1)
            {
              solved = true;
              break;
            }
        }
      }
      if (solved)
        computeSolution(sets, connections);
      else
        result = base::PlannerStatus::TIMEOUT;
    }
  }

  for (std::size_t i = 0 ; i < sets.size() ; ++i)
  {
    logDebug("Computed %u samples for constraints %u", (unsigned int)sets[i].size(), i);
    si_->freeStates(sets[i]);
  }
  si_->freeState(work_area);
  if (result)
    logInform("Successfully computed follow plan");
  else
    logInform("Unable to compute follow plan");
  return result;
}

void Follower::propagateStartInfo(std::size_t set_index, std::size_t elem_index,
                                  std::vector<std::vector<int> > &is_start,
                                  const std::vector< std::vector< std::vector<std::size_t> > > &connections)
{
  if (connections.size() <= set_index)
    return;
  
  const std::vector<std::size_t> &c = connections[set_index][elem_index];
  ++set_index;
  for (std::size_t i = 0 ; i < c.size() ; ++i)
  {
    is_start[set_index][c[i]] = 1;
    propagateStartInfo(set_index, c[i], is_start, connections);
  }
}

bool Follower::findSolutionPath(PathGeometric &path, std::size_t set_index, std::size_t elem_index,
                                const std::vector<std::vector<base::State*> > &sets,
                                const std::vector< std::vector< std::vector<std::size_t> > > &connections)
{ 
  if (set_index == connections.size()) // we are at the goal
  {
    path.append(sets[set_index][elem_index]);
    return true;
  }
  
  const std::vector<std::size_t> &c = connections[set_index][elem_index];

  for (std::size_t i = 0 ; i < c.size() ; ++i)
    if (findSolutionPath(path, set_index + 1, c[i], sets, connections))
    {
      path.append(sets[set_index][elem_index]);
      return true;
    }  

  return false;
}

void Follower::computeSolution(const std::vector<std::vector<base::State*> > &sets,
                               const std::vector< std::vector< std::vector<std::size_t> > > &connections)
{
  PathGeometric *pg = new PathGeometric(si_);
  bool found = false;
  for (std::size_t i = 0 ; !found && i < sets[0].size() ; ++i)
    found = findSolutionPath(*pg, 0, i, sets, connections);
  if (found)
  {
    pg->reverse();
    pdef_->addSolutionPath(base::PathPtr(pg));
  }
  else
  {
    delete pg;
  }
}

}
}

/** @endcond */

bool ompl_interface::ModelBasedPlanningContext::follow(double timeout, unsigned int count)
{
  ot::Profiler::ScopedBlock sblock("PlanningContext:Follow");
  ompl::time::point start = ompl::time::now();
  preSolve();
  
  bool result = false;

  og::Follower f(ompl_simple_setup_.getSpaceInformation());
  f.setProblemDefinition(ompl_simple_setup_.getProblemDefinition());  

  ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout);
  registerTerminationCondition(ptc);
  result = f.follow(follow_samplers_, ptc) == ompl::base::PlannerStatus::EXACT_SOLUTION;
  last_plan_time_ = ompl::time::seconds(ompl::time::now() - start);
  unregisterTerminationCondition();
  
  postSolve();

  return result;
}

void ompl_interface::ModelBasedPlanningContext::preSolve()
{
  // clear previously computed solutions
  ompl_simple_setup_.getProblemDefinition()->clearSolutionPaths();
  const ob::PlannerPtr planner = ompl_simple_setup_.getPlanner();
  if (planner)
    planner->clear();
  bool gls = ompl_simple_setup_.getGoal()->hasType(ob::GOAL_LAZY_SAMPLES);
  // just in case sampling is not started
  if (gls)
    static_cast<ob::GoalLazySamples*>(ompl_simple_setup_.getGoal().get())->startSampling();
  
  ompl_simple_setup_.getSpaceInformation()->getMotionValidator()->resetMotionCounter();
}

void ompl_interface::ModelBasedPlanningContext::postSolve()
{  
  bool gls = ompl_simple_setup_.getGoal()->hasType(ob::GOAL_LAZY_SAMPLES);
  if (gls)
    // just in case we need to stop sampling
    static_cast<ob::GoalLazySamples*>(ompl_simple_setup_.getGoal().get())->stopSampling();
  
  int v = ompl_simple_setup_.getSpaceInformation()->getMotionValidator()->getValidMotionCount();
  int iv = ompl_simple_setup_.getSpaceInformation()->getMotionValidator()->getInvalidMotionCount();
  logDebug("There were %d valid motions and %d invalid motions.", v, iv);
  
  if (ompl_simple_setup_.getProblemDefinition()->hasApproximateSolution())
    logWarn("Computed solution is approximate");
}

bool ompl_interface::ModelBasedPlanningContext::solve(double timeout, unsigned int count)
{
  ot::Profiler::ScopedBlock sblock("PlanningContext:Solve");

  ompl::time::point start = ompl::time::now();
  preSolve();
  
  bool result = false;
  if (count <= 1)
  {
    logDebug("%s: Solving the planning problem once...", name_.c_str());
    ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout - ompl::time::seconds(ompl::time::now() - start));
    registerTerminationCondition(ptc);
    result = ompl_simple_setup_.solve(ptc) == ompl::base::PlannerStatus::EXACT_SOLUTION;
    last_plan_time_ = ompl_simple_setup_.getLastPlanComputationTime();
    unregisterTerminationCondition();
  }
  else
  {
    logDebug("%s: Solving the planning problem %u times...", name_.c_str(), count);
    ompl_parallel_plan_.clearHybridizationPaths();
    if (count <= max_planning_threads_)
    {
      ompl_parallel_plan_.clearPlanners();
      if (ompl_simple_setup_.getPlannerAllocator())
	for (unsigned int i = 0 ; i < count ; ++i)
	  ompl_parallel_plan_.addPlannerAllocator(ompl_simple_setup_.getPlannerAllocator());
      else
	for (unsigned int i = 0 ; i < count ; ++i)
	  ompl_parallel_plan_.addPlanner(ompl::geometric::getDefaultPlanner(ompl_simple_setup_.getGoal())); 

      ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout - ompl::time::seconds(ompl::time::now() - start));
      registerTerminationCondition(ptc);
      result = ompl_parallel_plan_.solve(ptc, 1, count, true) == ompl::base::PlannerStatus::EXACT_SOLUTION;
      last_plan_time_ = ompl::time::seconds(ompl::time::now() - start);
      unregisterTerminationCondition();
    }
    else
    {
      ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout - ompl::time::seconds(ompl::time::now() - start));
      registerTerminationCondition(ptc);
      int n = count / max_planning_threads_;
      result = true;
      for (int i = 0 ; i < n && ptc() == false ; ++i)
      {
	ompl_parallel_plan_.clearPlanners();
	if (ompl_simple_setup_.getPlannerAllocator())
	  for (unsigned int i = 0 ; i < max_planning_threads_ ; ++i)
	    ompl_parallel_plan_.addPlannerAllocator(ompl_simple_setup_.getPlannerAllocator());
	else
	  for (unsigned int i = 0 ; i < max_planning_threads_ ; ++i)
	    ompl_parallel_plan_.addPlanner(og::getDefaultPlanner(ompl_simple_setup_.getGoal()));
	bool r = ompl_parallel_plan_.solve(ptc, 1, max_planning_threads_, true) == ompl::base::PlannerStatus::EXACT_SOLUTION;
	result = result && r; 
      }
      n = count % max_planning_threads_;
      if (n && ptc() == false)
      {
	ompl_parallel_plan_.clearPlanners();
	if (ompl_simple_setup_.getPlannerAllocator())
	  for (int i = 0 ; i < n ; ++i)
	    ompl_parallel_plan_.addPlannerAllocator(ompl_simple_setup_.getPlannerAllocator());
	else
	  for (int i = 0 ; i < n ; ++i)
	    ompl_parallel_plan_.addPlanner(og::getDefaultPlanner(ompl_simple_setup_.getGoal()));
	bool r = ompl_parallel_plan_.solve(ptc, 1, n, true) == ompl::base::PlannerStatus::EXACT_SOLUTION;
	result = result && r;
      }
      last_plan_time_ = ompl::time::seconds(ompl::time::now() - start);
      unregisterTerminationCondition();
    }
  }
  
  postSolve();
    
  return result;
}

void ompl_interface::ModelBasedPlanningContext::registerTerminationCondition(const ob::PlannerTerminationCondition &ptc)
{
  boost::mutex::scoped_lock slock(ptc_lock_);
  ptc_ = &ptc;
}

void ompl_interface::ModelBasedPlanningContext::unregisterTerminationCondition()
{ 
  boost::mutex::scoped_lock slock(ptc_lock_);
  ptc_ = NULL;
}

void ompl_interface::ModelBasedPlanningContext::terminateSolve()
{
  boost::mutex::scoped_lock slock(ptc_lock_);
  if (ptc_)
    ptc_->terminate();
}
