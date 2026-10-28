#pragma once

#include <vector>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <iomanip>
#include <sstream>

/**
 * @file analytical_solver.hpp
 * @brief Exact Analytical M/M/c/m Queueing Theory Solver for Pizzeria Active Oven Management.
 * 
 * Developed by Role 3 (Theoretical Modeler & Technical Writer).
 * Computes exact steady-state state probabilities and quality-of-service (QoS)
 * metrics for multi-server queueing systems with finite buffers:
 *   - c: Active ovens (1 <= c <= 10)
 *   - m: Waiting queue capacity (m = 10)
 *   - K: Total capacity = c + m
 * 
 * Strictly preserves existing project headers and can be directly included
 * by Participant 1 and Participant 2.
 */

namespace queueing_theory {

struct AnalyticalMetrics {
    int c;                          ///< Number of active parallel ovens (servers)
    int m;                          ///< Maximum queue capacity
    int capacity_K;                 ///< Total system capacity (c + m)
    double lambda_rate;             ///< Arrival rate (orders per unit time)
    double mu_rate;                 ///< Service rate per oven (orders per unit time)
    double rho;                     ///< Offered load (lambda / mu)
    double psi;                     ///< Channel utilization factor (rho / c)
    double p0;                      ///< System idle probability P_0
    double p_reject;                ///< Loss probability P_{c+m}
    double relative_throughput;     ///< Q = 1 - P_reject
    double absolute_throughput;     ///< A = lambda * Q
    double avg_busy_servers;        ///< L_s = A / mu
    double server_utilization;      ///< eta = L_s / c
    double avg_queue_length;        ///< L_q
    double avg_system_entities;     ///< L = L_s + L_q
    double avg_wait_time_queue;     ///< W_q = L_q / A
    double avg_time_in_system;      ///< W = W_q + 1 / mu
    std::vector<double> state_probs;///< P_k for k = 0, ..., c + m
};

struct EconomicEvaluation {
    double duration_hours;
    double expected_arrivals;
    double expected_served;
    double expected_lost;
    double gross_revenue;
    double oven_operational_cost;
    double lost_penalty_cost;
    double fixed_overhead;
    double net_profit;
};

class AnalyticalMMcmSolver {
public:
    /**
     * @brief Construct an M/M/c/m solver instance.
     * @param c Number of active parallel servers (ovens). Must be >= 1.
     * @param m Finite queue capacity. Must be >= 0. Default is 10.
     */
    AnalyticalMMcmSolver(int c, int m = 10) : c_(c), m_(m), capacity_K_(c + m) {
        if (c < 1) {
            throw std::invalid_argument("Active ovens c must be >= 1");
        }
        if (m < 0) {
            throw std::invalid_argument("Queue capacity m must be >= 0");
        }
    }

    /**
     * @brief Computes all steady-state performance metrics for given arrival and service rates.
     * @param lambda_rate Incoming arrival rate (lambda >= 0).
     * @param mu_rate Service rate per single server (mu > 0).
     * @return AnalyticalMetrics filled structure.
     */
    AnalyticalMetrics solve(double lambda_rate, double mu_rate) const {
        if (lambda_rate < 0.0) {
            throw std::invalid_argument("Arrival rate lambda cannot be negative");
        }
        if (mu_rate <= 0.0) {
            throw std::invalid_argument("Service rate mu must be strictly positive");
        }

        AnalyticalMetrics metrics{};
        metrics.c = c_;
        metrics.m = m_;
        metrics.capacity_K = capacity_K_;
        metrics.lambda_rate = lambda_rate;
        metrics.mu_rate = mu_rate;
        metrics.state_probs.resize(capacity_K_ + 1, 0.0);

        if (std::abs(lambda_rate) < 1e-12) {
            metrics.rho = 0.0;
            metrics.psi = 0.0;
            metrics.p0 = 1.0;
            metrics.p_reject = 0.0;
            metrics.relative_throughput = 1.0;
            metrics.absolute_throughput = 0.0;
            metrics.avg_busy_servers = 0.0;
            metrics.server_utilization = 0.0;
            metrics.avg_queue_length = 0.0;
            metrics.avg_system_entities = 0.0;
            metrics.avg_wait_time_queue = 0.0;
            metrics.avg_time_in_system = 1.0 / mu_rate;
            metrics.state_probs[0] = 1.0;
            return metrics;
        }

        const double rho = lambda_rate / mu_rate;
        const double psi = rho / static_cast<double>(c_);
        metrics.rho = rho;
        metrics.psi = psi;

        // Unnormalized terms: term[k] = P_k / P_0
        std::vector<double> terms(capacity_K_ + 1, 1.0);
        double current_term = 1.0;

        // Channel saturation region: 1 <= k <= c
        for (int k = 1; k <= c_; ++k) {
            current_term *= (rho / static_cast<double>(k));
            terms[k] = current_term;
        }

        // Queueing region: c < k <= c + m
        double queue_term = terms[c_];
        for (int j = 1; j <= m_; ++j) {
            queue_term *= psi;
            terms[c_ + j] = queue_term;
        }

        // Normalization: sum(P_k) = 1 => P_0 = 1 / sum(terms)
        double sum_terms = 0.0;
        for (double t : terms) {
            sum_terms += t;
        }
        const double p0 = 1.0 / sum_terms;
        metrics.p0 = p0;

        for (int k = 0; k <= capacity_K_; ++k) {
            metrics.state_probs[k] = terms[k] * p0;
        }

        metrics.p_reject = metrics.state_probs[capacity_K_];
        metrics.relative_throughput = 1.0 - metrics.p_reject;
        metrics.absolute_throughput = lambda_rate * metrics.relative_throughput;

        // Flow conservation: L_s = A / mu
        metrics.avg_busy_servers = metrics.absolute_throughput / mu_rate;
        metrics.server_utilization = metrics.avg_busy_servers / static_cast<double>(c_);

        // Average queue length L_q = sum_{j=1}^m j * P_{c+j}
        double lq = 0.0;
        for (int j = 1; j <= m_; ++j) {
            lq += static_cast<double>(j) * metrics.state_probs[c_ + j];
        }
        metrics.avg_queue_length = lq;
        metrics.avg_system_entities = metrics.avg_busy_servers + metrics.avg_queue_length;

        // Little's Law: W_q = L_q / A
        if (metrics.absolute_throughput > 1e-12) {
            metrics.avg_wait_time_queue = metrics.avg_queue_length / metrics.absolute_throughput;
        } else {
            metrics.avg_wait_time_queue = 0.0;
        }
        metrics.avg_time_in_system = metrics.avg_wait_time_queue + (1.0 / mu_rate);

        return metrics;
    }

    /**
     * @brief Computes economic performance over a specified duration in hours.
     */
    static EconomicEvaluation evaluate_economics(
        const AnalyticalMetrics& m,
        double duration_hours,
        double revenue_per_order = 250.0,
        double oven_cost_per_hour = 60.0,
        double idle_power_ratio = 0.5,
        double lost_penalty = 50.0,
        double fixed_overhead_per_hour = 120.0
    ) {
        EconomicEvaluation eval{};
        eval.duration_hours = duration_hours;
        eval.expected_arrivals = m.lambda_rate * duration_hours;
        eval.expected_served = m.absolute_throughput * duration_hours;
        eval.expected_lost = eval.expected_arrivals - eval.expected_served;
        eval.gross_revenue = eval.expected_served * revenue_per_order;

        double busy_cost = m.avg_busy_servers * oven_cost_per_hour * duration_hours;
        double idle_servers = std::max(0.0, static_cast<double>(m.c) - m.avg_busy_servers);
        double idle_cost = idle_servers * oven_cost_per_hour * idle_power_ratio * duration_hours;

        eval.oven_operational_cost = busy_cost + idle_cost;
        eval.lost_penalty_cost = eval.expected_lost * lost_penalty;
        eval.fixed_overhead = fixed_overhead_per_hour * duration_hours;
        eval.net_profit = eval.gross_revenue - (eval.oven_operational_cost + eval.lost_penalty_cost + eval.fixed_overhead);

        return eval;
    }

private:
    int c_;
    int m_;
    int capacity_K_;
};

} // namespace queueing_theory
