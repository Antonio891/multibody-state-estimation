#include <sparsembs/CModelDefinition.h>
#include <sparsembs/CAssembledModelRigid.h>
#include <sparsembs/dynamic-simulators.h>
#include <fstream>

using namespace sparsembs;
using namespace Eigen;
using namespace mrpt::math;
using namespace mrpt;
using namespace std;

#define USE_BAUMGARTEN_STABILIZATION 1

const double dummy_zero = 0;

TSimulationState::TSimulationState(const CAssembledRigidModel* arm_)
	: t(0), arm(arm_)
{
}

// Virtual destructor: requird for virtual bases
CDynamicSimulatorBase::~CDynamicSimulatorBase() {}

/** A class factory, creates a dynamic simulator from a string with the class
 * name: "CDynamicSimulator_Lagrange_LU_dense",
 * "CDynamicSimulator_Lagrange_UMFPACK", ...
 */
CDynamicSimulatorBase::Ptr CDynamicSimulatorBase::Create(
	const std::string& name,
	const std::shared_ptr<CAssembledRigidModel> arm_ptr)
{
	if (name == "CDynamicSimulator_Lagrange_LU_dense")
		return Ptr(new CDynamicSimulator_Lagrange_LU_dense(arm_ptr));
	else if (name == "CDynamicSimulator_Lagrange_CHOLMOD")
		return Ptr(new CDynamicSimulator_Lagrange_CHOLMOD(arm_ptr));
	else if (name == "CDynamicSimulator_Lagrange_UMFPACK")
		return Ptr(new CDynamicSimulator_Lagrange_UMFPACK(arm_ptr));
	else if (name == "CDynamicSimulator_Lagrange_KLU")
		return Ptr(new CDynamicSimulator_Lagrange_KLU(arm_ptr));
	else
		THROW_EXCEPTION("Unknown dynamic simulator class name: " + name);
}

// ---------------------------------------------------------------------------------------------
//  Solver: Virtual base class
// ---------------------------------------------------------------------------------------------
CDynamicSimulatorBase::CDynamicSimulatorBase(
	std::shared_ptr<CAssembledRigidModel> arm_ptr)
	: m_arm_ptr(arm_ptr), m_arm(arm_ptr.get()), m_init(false)
{
	ASSERT_(arm_ptr);
}

/** Solve for the current accelerations */
void CDynamicSimulatorBase::solve_ddotq(
	double t, VectorXd& ddot_q, VectorXd* lagrangre)
{
	ASSERT_(m_init);
	this->internal_solve_ddotq(t, ddot_q, lagrangre);
}

/** Prepare the linear systems and anything else required to really call
 * solve_ddotq() */
void CDynamicSimulatorBase::prepare()
{
	this->internal_prepare();
	m_init = true;
}

void do_nothing(const TSimulationStateRef st) {}

CDynamicSimulatorBase::TParameters::TParameters()
	: ode_solver(ODE_Euler),
	  time_step(1e-3),
	  user_callback(simul_callback_t(do_nothing))
{
}

/** Runs a dynamic simulation for a given time span */
double CDynamicSimulatorBase::run(const double t_ini, const double t_end)
{
	ASSERT_(t_end >= t_ini);
	ASSERT_(m_init);

	if (t_ini == t_end) return t_end;  // Nothing to do.

	// Fill constant data
	TSimulationState sim_state(m_arm);

	const double t_step = std::min(t_end - t_ini, params.time_step);
	const double t_step2 = t_step * 0.5;
	const double t_step6 = t_step / 6.0;

	double t;  // Declared here so we know the final "time":
	for (t = t_ini; t < t_end; t += t_step)
	{
		// Log sensor points:
		// ------------------------------
		for (std::list<TSensorData>::iterator it = m_sensors.begin();
			 it != m_sensors.end(); ++it)
		{
			TSensorData& sd = *it;
			sd.log.push_back(timestamped_point_t(
				t, TPointState(
					   mrpt::math::TPoint2D(*sd.pos[0], *sd.pos[1]),
					   mrpt::math::TPoint2D(*sd.vel[0], *sd.vel[1]))));
		}

		timelog.enter("mbs.run_complete_timestep");

		// Integrate:
		// ------------------------------
		this->pre_iteration(t);

		const bool custom_integrator =
			this->internal_integrate(t, t_step, params.ode_solver);

		if (!custom_integrator)
		{
			// Generic integrator

			switch (params.ode_solver)
			{
				// -------------------------------------------
				case ODE_Euler:
				{
					this->internal_solve_ddotq(t, ddotq1);
					m_arm->m_q += t_step * m_arm->m_dotq;
					m_arm->m_dotq += t_step * ddotq1;
				}
				break;

				// -------------------------------------------
				case ODE_RK4:
				{
					q0 = m_arm->m_q;  // Make backup copy of state (velocities
									  // will be in "v1")

					// k1 = f(t,y);
					// cur_time = t;
					v1 = m_arm->m_dotq;
					// No change needed: m_arm->m_q = q0;
					this->internal_solve_ddotq(t, ddotq1);

					// k2 = f(t+At/2,y+At/2*k1)
					// cur_time = t + t_step2;
					m_arm->m_dotq =
						v1 +
						t_step2 *
							ddotq1;  // \dot{q}= \dot{q}_0 + At/2 * \ddot{q}_1
					m_arm->m_q = q0 + t_step2 * v1;
					v2 = m_arm->m_dotq;
					this->internal_solve_ddotq(t + t_step2, ddotq2);

					// k3 = f(t+At/2,y+At/2*k2)
					// cur_time = t + t_step2;
					m_arm->m_dotq =
						v1 +
						t_step2 *
							ddotq2;  // \dot{q}= \dot{q}_0 + At/2 * \ddot{q}_2
					m_arm->m_q = q0 + t_step2 * v2;
					v3 = m_arm->m_dotq;
					this->internal_solve_ddotq(t + t_step2, ddotq3);

					// k4 = f(t+At  ,y+At*k3)
					// cur_time = t + t_step;
					m_arm->m_dotq = v1 + t_step * ddotq3;
					m_arm->m_q = q0 + t_step * v3;
					v4 = m_arm->m_dotq;
					this->internal_solve_ddotq(t + t_step, ddotq4);

					// Runge-Kutta 4th order formula:
					m_arm->m_q = q0 + t_step6 * (v1 + 2 * v2 + 2 * v3 + v4);
					m_arm->m_dotq = v1 + t_step6 * (ddotq1 + 2 * ddotq2 +
													2 * ddotq3 + ddotq4);
				}
				break;

				// Implicit trapezoidal integration rule:
				// -------------------------------------------
				case ODE_Trapezoidal:
				{
					const double t_step_sq = t_step * t_step;

					const size_t MAX_ITERS = 10;
					const double QDIFF_MAX = 1e-10;
					double qdiff = 10 * QDIFF_MAX;

					// Keep the initial state:
					const Eigen::VectorXd q0 = m_arm->m_q;
					const Eigen::VectorXd dq0 = m_arm->m_dotq;

					// First attempt:
					Eigen::VectorXd ddq0;
					this->internal_solve_ddotq(t, ddq0);

					Eigen::VectorXd q_new =
						q0 + t_step * dq0 + 0.5 * t_step_sq * ddq0;
					Eigen::VectorXd dq_new = dq0 + t_step * ddq0;

					Eigen::VectorXd q_old = q_new;
					// Solve at the new predicted state "t=k+1":
					m_arm->m_q = q_new;
					m_arm->m_dotq = dq_new;

					Eigen::VectorXd ddq_mid;
					size_t iter;
					for (iter = 0; iter < MAX_ITERS && qdiff > QDIFF_MAX;
						 iter++)
					{
						// Store previous state for comparing the progress of
						// the iterative method:
						q_old = q_new;

						// Solve at the new predicted state "t=k+1":
						this->internal_solve_ddotq(t + t_step, ddotq1);

						// integrator (trapezoidal rule)
						// -------------------------------
						ddq_mid = (ddotq1 + ddq0) * 0.5;
						q_new = q0 + t_step * dq0 + 0.5 * t_step_sq * ddq_mid;
						dq_new = dq0 + t_step * ddq_mid;

						// check progress:
						qdiff = (q_old - q_new).norm();

						// Solve at the new predicted state "t=k+1":
						m_arm->m_q = q_new;
						m_arm->m_dotq = dq_new;
					}

					ASSERTMSG_(
						iter < MAX_ITERS, "Trapezoidal convergence failed!");

					timelog.registerUserMeasure("trapezoidal.iters", iter);
				}
				break;

				default:
					THROW_EXCEPTION("Unknown value for params.ode_solver");
			};
		}

		this->post_iteration(t);

		timelog.leave("mbs.run_complete_timestep");

		// User-callback:
		// ------------------------------
		sim_state.t = t;
		params.user_callback(sim_state);
	}

	return t;
}

/** Runs a dynamic simulation for a given time span */
void CDynamicSimulatorBase::build_RHS(double* Q, double* c)
{
	const size_t nConstraints = m_arm->m_Phi.size();

	// Q part:
	m_arm->builGeneralizedForces(Q);

	// "c" part:
	if (c)
	{
		// "-\dot{Phi_q} * \dot{q}"
		for (size_t i = 0; i < nConstraints; i++)
		{
			// c[i] = sum_k( -dot{Phi_q}[i,k] * dot_q[k] )
			double ci = 0;
			const TCompressedRowSparseMatrix::row_t row_i =
				m_arm->m_dotPhi_q.matrix[i];
			for (TCompressedRowSparseMatrix::row_t::const_iterator itCol =
					 row_i.begin();
				 itCol != row_i.end(); ++itCol)
			{
				const size_t col = itCol->first;
				ci -= itCol->second * m_arm->m_dotq[col];
			}

			// Save c_i in its place within "c":
			c[i] = ci;
		}

#if USE_BAUMGARTEN_STABILIZATION
		// Use Baumgarten Stabilization
		//  c= -\dot{Phi_q} * \dot{q}  - 2*eps*omega*dotPhi - omega^2 * Phi
		const double epsilon = 1;
		const double omega = 10;
		for (size_t i = 0; i < nConstraints; i++)
			c[i] -= 2 * epsilon * omega * m_arm->m_dotPhi[i] +
					omega * omega * m_arm->m_Phi[i];
#endif
	}
}

/** Add a "sensor" that grabs the position of a given point.
 * \sa saveSensorLogsToFile
 */
void CDynamicSimulatorBase::addPointSensor(const size_t pnt_index)
{
	ASSERT_(pnt_index < m_arm->m_parent.getPointCount());

	// Create entry
	m_sensors.push_back(TSensorData());
	TSensorData& sd = m_sensors.back();

	// Init pointers, etc:
	sd.pnt_index = pnt_index;

	const TMBSPoint* mbs_point = &m_arm->m_parent.getPointInfo(pnt_index);
	const TPoint2DOF& point_dof = m_arm->getPoints2DOFs()[pnt_index];

	// Get references to the point coordinates (either fixed or variables in q):
	sd.pos[0] =
		((point_dof.dof_x != INVALID_DOF) ? &m_arm->m_q[point_dof.dof_x]
										  : &mbs_point->coords.x);
	sd.pos[1] =
		((point_dof.dof_y != INVALID_DOF) ? &m_arm->m_q[point_dof.dof_y]
										  : &mbs_point->coords.y);

	// Get references to the point velocities:
	sd.vel[0] =
		((point_dof.dof_x != INVALID_DOF) ? &m_arm->m_dotq[point_dof.dof_x]
										  : &dummy_zero);
	sd.vel[1] =
		((point_dof.dof_y != INVALID_DOF) ? &m_arm->m_dotq[point_dof.dof_y]
										  : &dummy_zero);
}

/** Save all logged data to a text file, loadable from MATLAB with "load()".
 * \return false on any error, true if all go ok.
 */
bool CDynamicSimulatorBase::saveSensorLogsToFile(
	const std::string& filename) const
{
	std::ofstream f(filename.c_str());
	if (!f.is_open()) return false;

	// Header:
	size_t N = 0;
	f << "% Time ";
	for (std::list<TSensorData>::const_iterator it = m_sensors.begin();
		 it != m_sensors.end(); ++it)
	{
		const TSensorData& sd = *it;
		N = sd.log.size();
		f << "\t      x" << sd.pnt_index << "\t      y" << sd.pnt_index
		  << "\t      vx" << sd.pnt_index << "\t      vy" << sd.pnt_index;
	}
	f << "\n"
		 "% "
		 "---------------------------------------------------------------------"
		 "------------------\n";

	if (m_sensors.empty()) return true;

	// Data:
	std::string s;
	for (size_t i = 0; i < N; i++)
	{
		s = mrpt::format("%f\t", m_sensors.begin()->log[i].first);

		for (std::list<TSensorData>::const_iterator it = m_sensors.begin();
			 it != m_sensors.end(); ++it)
		{
			const TSensorData& sd = *it;
			s += mrpt::format(
				"%f\t %f\t %f\t %f\t ", sd.log[i].second.pos.x,
				sd.log[i].second.pos.y, sd.log[i].second.vel.x,
				sd.log[i].second.vel.y);
		}
		s += std::string("\n");
		f << s;
	}

	return true;
}
