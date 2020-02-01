// Example of numerical integration using factor graphs:
// ------------------------------------------------------------
#include <sparsembs/FactorTrapInt.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/nonlinear/Values.h>
#include <iostream>

using namespace std;

void test_numerical_integration()
{
	using gtsam::symbol_shorthand::V;
	using gtsam::symbol_shorthand::X;
	using namespace sparsembs;

	// Create the factor graph:
	gtsam::NonlinearFactorGraph graph;
	gtsam::Values initValues;

	// Add factors:
	// Create factor noises:
	auto noise = gtsam::noiseModel::Diagonal::Sigmas(gtsam::Vector2(1, 1));
	auto noise_prior =
		gtsam::noiseModel::Diagonal::Sigmas(gtsam::Vector2(1, 1));

	const double dt = 1.0;

	// Create Trapezoidal Integrator factors:
	graph.emplace_shared<FactorTrapInt>(dt, noise, X(1), X(2), V(1), V(2));
	graph.emplace_shared<FactorTrapInt>(dt, noise, X(2), X(3), V(2), V(3));

	// Create null vector:
	const state_t zeros = gtsam::Vector(gtsam::Vector2(0.0, 0.0));

	// Create Prior vectors:
	const state_t prior_x1 = gtsam::Vector(gtsam::Vector2(0.0, 0.0));
	const state_t prior_v1 = gtsam::Vector(gtsam::Vector2(1.0, 2.0));
	const state_t prior_v2 = gtsam::Vector(gtsam::Vector2(2.0, 3.0));
	const state_t prior_v3 = gtsam::Vector(gtsam::Vector2(2.0, 3.0));

	// Create Prior factors:
	graph.emplace_shared<gtsam::PriorFactor<state_t>>(
		X(1), prior_x1, noise_prior);
	graph.emplace_shared<gtsam::PriorFactor<state_t>>(
		V(1), prior_v1, noise_prior);
	graph.emplace_shared<gtsam::PriorFactor<state_t>>(
		V(2), prior_v2, noise_prior);
	graph.emplace_shared<gtsam::PriorFactor<state_t>>(
		V(3), prior_v3, noise_prior);


	// Create initial estimates:
	initValues.insert(X(1), zeros);
	initValues.insert(X(2), zeros);
	initValues.insert(X(3), zeros);
	initValues.insert(V(1), zeros);
	initValues.insert(V(2), zeros);
	initValues.insert(V(3), zeros);

	// Run optimizer:
	graph.print("Factor graph: ");
	initValues.print("initValues: ");
	gtsam::LevenbergMarquardtOptimizer optimizer(graph, initValues);

	const auto& optimValues = optimizer.optimize();

	// Process results:
	optimValues.print("optimValues");
}

int main()
{
	try
	{
		test_numerical_integration();
	}
	catch (const std::exception & e)
	{
		std::cerr << "Error: " << e.what() << "\n";
	}
}
