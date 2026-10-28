"""Core Analytical Solver for M/M/c/m (Erlang Delay-Loss) Queueing Systems.

This module implements exact closed-form and steady-state matrix/probabilistic
calculations for multi-server queueing systems with finite buffer capacity:
    - Number of servers (active ovens): c (1 <= c <= 10)
    - Queue capacity (waiting orders): m (m = 10)
    - System capacity: K = c + m
    - Markovian arrival process: Poisson with rate lambda
    - Markovian service process: Exponential with rate mu per server
"""

from __future__ import annotations
import math
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class QueueingMetrics:
    """Immutable data container holding comprehensive theoretical metrics for an M/M/c/m system.

    Attributes:
        c: Number of active parallel servers (ovens).
        m: Finite queue capacity (maximum orders waiting).
        capacity_K: Total system capacity (c + m).
        lambda_rate: Order arrival rate (orders / unit time).
        mu_rate: Service completion rate per active server (orders / unit time).
        rho: Offered system load (traffic intensity), rho = lambda / mu.
        psi: Server load factor (traffic intensity per channel), psi = rho / c.
        p0: System idle probability (probability of 0 orders in system).
        p_reject: Loss / rejection probability (P_{c+m}).
        relative_throughput: Fraction of arrivals admitted and served, Q = 1 - P_reject.
        absolute_throughput: Admitted arrival rate, A = lambda * (1 - P_reject).
        busy_servers: Average number of concurrently busy servers, L_s = A / mu.
        server_utilization: Server utilization factor, eta = L_s / c = psi * (1 - P_reject).
        avg_queue_length: Expected number of orders waiting in queue, L_q.
        avg_system_entities: Expected total orders in system, L = L_s + L_q.
        avg_wait_time_queue: Mean waiting time in queue for admitted orders, W_q = L_q / A.
        avg_time_in_system: Mean total time in system for admitted orders, W = W_q + 1/mu.
        state_probabilities: Dictionary mapping state index k in [0, c+m] to probability P_k.
    """

    c: int
    m: int
    capacity_K: int
    lambda_rate: float
    mu_rate: float
    rho: float
    psi: float
    p0: float
    p_reject: float
    relative_throughput: float
    absolute_throughput: float
    busy_servers: float
    server_utilization: float
    avg_queue_length: float
    avg_system_entities: float
    avg_wait_time_queue: float
    avg_time_in_system: float
    state_probabilities: Dict[int, float]

    def summary_dict(self) -> Dict[str, float]:
        """Return a human-readable dictionary of the primary key performance indicators."""
        return {
            "Active Ovens (c)": float(self.c),
            "Queue Capacity (m)": float(self.m),
            "System Capacity (K)": float(self.capacity_K),
            "Arrival Rate (lambda)": self.lambda_rate,
            "Service Rate (mu)": self.mu_rate,
            "Offered Load (rho)": self.rho,
            "Channel Load (psi)": self.psi,
            "Idle Prob (P0)": self.p0,
            "Rejection Prob (P_reject)": self.p_reject,
            "Relative Throughput (Q)": self.relative_throughput,
            "Absolute Throughput (A)": self.absolute_throughput,
            "Average Busy Ovens (L_s)": self.busy_servers,
            "Oven Utilization (eta)": self.server_utilization,
            "Average Queue Length (L_q)": self.avg_queue_length,
            "Average Orders in System (L)": self.avg_system_entities,
            "Average Wait Time in Queue (W_q)": self.avg_wait_time_queue,
            "Average System Time (W)": self.avg_time_in_system,
        }


class MMcmModel:
    """Analytical mathematical model for an M/M/c/m finite-buffer queueing system.

    Implements exact steady-state solutions derived from the continuous-time
    Markov birth-death process (Kolmogorov equations).
    """

    def __init__(self, c: int, m: int = 10) -> None:
        """Initialize M/M/c/m solver parameters.

        Args:
            c: Number of parallel active ovens (c >= 1).
            m: Maximum queue capacity (m >= 0). Default is 10 as specified for Option 4.
        """
        if c < 1:
            raise ValueError(f"Number of active ovens c must be >= 1, got {c}.")
        if m < 0:
            raise ValueError(f"Queue capacity m must be >= 0, got {m}.")
        self.c = int(c)
        self.m = int(m)
        self.capacity_K = self.c + self.m

    def compute_state_probabilities(
        self, lambda_rate: float, mu_rate: float
    ) -> Tuple[float, Dict[int, float]]:
        """Compute the full probability distribution {P_k} for k in [0, c+m].

        Formulas:
            rho = lambda / mu
            psi = rho / c

            For 0 <= k <= c:
                P_k = (rho^k / k!) * P_0
            For c < k <= c + m:
                P_k = (rho^c / c!) * psi^(k - c) * P_0

            P_0 = 1 / [ sum_{k=0}^c (rho^k / k!) + (rho^c / c!) * sum_{j=1}^m psi^j ]

        Args:
            lambda_rate: Order arrival rate (lambda >= 0).
            mu_rate: Service rate per single oven (mu > 0).

        Returns:
            Tuple of (P_0, state_probabilities_dict).
        """
        if lambda_rate < 0.0:
            raise ValueError(f"Arrival rate lambda must be >= 0, got {lambda_rate}.")
        if mu_rate <= 0.0:
            raise ValueError(f"Service rate mu must be > 0, got {mu_rate}.")

        # Handle zero arrival edge case
        if math.isclose(lambda_rate, 0.0):
            probs = {0: 1.0}
            for k in range(1, self.capacity_K + 1):
                probs[k] = 0.0
            return 1.0, probs

        rho = lambda_rate / mu_rate
        psi = rho / float(self.c)

        # Compute unnormalized terms: term[k] = P_k / P_0
        terms: List[float] = [1.0]  # k = 0 -> term = 1.0

        # Channel saturation phase: 1 <= k <= c
        current_term = 1.0
        for k in range(1, self.c + 1):
            current_term *= rho / float(k)
            terms.append(current_term)

        # Queueing phase: c < k <= c + m
        # term[c + j] = term[c] * psi^j
        term_c = terms[self.c]
        current_queue_term = term_c
        for j in range(1, self.m + 1):
            current_queue_term *= psi
            terms.append(current_queue_term)

        # Normalization: sum(P_k) = 1  =>  P_0 = 1 / sum(terms)
        sum_terms = sum(terms)
        p0 = 1.0 / sum_terms

        probs = {k: terms[k] * p0 for k in range(self.capacity_K + 1)}

        return p0, probs

    def calculate_metrics(self, lambda_rate: float, mu_rate: float) -> QueueingMetrics:
        """Calculate complete analytical queueing metrics for given arrival and service rates.

        Args:
            lambda_rate: Arrival rate (orders per unit time, e.g. per minute or per hour).
            mu_rate: Service rate per oven (orders per unit time).

        Returns:
            An instantiated QueueingMetrics object with all derived metrics.
        """
        if lambda_rate < 0.0:
            raise ValueError(f"Arrival rate lambda must be >= 0, got {lambda_rate}.")
        if mu_rate <= 0.0:
            raise ValueError(f"Service rate mu must be > 0, got {mu_rate}.")

        # Zero arrival boundary case
        if math.isclose(lambda_rate, 0.0):
            probs = {0: 1.0}
            for k in range(1, self.capacity_K + 1):
                probs[k] = 0.0
            return QueueingMetrics(
                c=self.c,
                m=self.m,
                capacity_K=self.capacity_K,
                lambda_rate=0.0,
                mu_rate=mu_rate,
                rho=0.0,
                psi=0.0,
                p0=1.0,
                p_reject=0.0,
                relative_throughput=1.0,
                absolute_throughput=0.0,
                busy_servers=0.0,
                server_utilization=0.0,
                avg_queue_length=0.0,
                avg_system_entities=0.0,
                avg_wait_time_queue=0.0,
                avg_time_in_system=1.0 / mu_rate,
                state_probabilities=probs,
            )

        rho = lambda_rate / mu_rate
        psi = rho / float(self.c)

        p0, probs = self.compute_state_probabilities(lambda_rate, mu_rate)

        # Rejection probability: system is completely full (state c + m)
        p_reject = probs[self.capacity_K]

        # Relative and absolute throughput
        relative_throughput = 1.0 - p_reject
        absolute_throughput = lambda_rate * relative_throughput

        # Average busy servers L_s:
        # In steady state, by conservation of flow, L_s = A / mu = rho * (1 - P_reject)
        busy_servers = absolute_throughput / mu_rate
        server_utilization = busy_servers / float(self.c)

        # Average queue length L_q = sum_{j=1}^m j * P_{c+j}
        # Closed-form formula with numerical fallback:
        if self.m == 0:
            avg_queue_length = 0.0
        elif math.isclose(psi, 1.0, rel_tol=1e-9, abs_tol=1e-9):
            # When psi == 1: sum_{j=1}^m j = m*(m+1)/2
            term_c = probs[self.c]
            avg_queue_length = term_c * (self.m * (self.m + 1) / 2.0)
        else:
            # Closed form:
            term_c = probs[self.c]
            numerator = psi * (1.0 - (self.m + 1.0) * (psi**self.m) + self.m * (psi ** (self.m + 1)))
            denominator = (1.0 - psi) ** 2
            avg_queue_length = term_c * (numerator / denominator)

        # Double check with direct summation for absolute numerical precision
        direct_lq = sum(j * probs[self.c + j] for j in range(1, self.m + 1))
        # If any numerical instability in closed form (e.g. psi extremely close to 1), use direct summation
        if not math.isfinite(avg_queue_length) or abs(avg_queue_length - direct_lq) > 1e-6:
            avg_queue_length = direct_lq

        # Average orders in the entire system L = L_s + L_q
        avg_system_entities = busy_servers + avg_queue_length

        # Average waiting time in queue W_q by Little's Law: W_q = L_q / A
        if absolute_throughput > 1e-12:
            avg_wait_time_queue = avg_queue_length / absolute_throughput
        else:
            avg_wait_time_queue = 0.0

        # Average total time in system W = W_q + 1 / mu = L / A
        avg_time_in_system = avg_wait_time_queue + (1.0 / mu_rate)

        return QueueingMetrics(
            c=self.c,
            m=self.m,
            capacity_K=self.capacity_K,
            lambda_rate=lambda_rate,
            mu_rate=mu_rate,
            rho=rho,
            psi=psi,
            p0=p0,
            p_reject=p_reject,
            relative_throughput=relative_throughput,
            absolute_throughput=absolute_throughput,
            busy_servers=busy_servers,
            server_utilization=server_utilization,
            avg_queue_length=avg_queue_length,
            avg_system_entities=avg_system_entities,
            avg_wait_time_queue=avg_wait_time_queue,
            avg_time_in_system=avg_time_in_system,
            state_probabilities=probs,
        )


def solve_mmcm(c: int, m: int, lambda_rate: float, mu_rate: float) -> QueueingMetrics:
    """Convenience functional interface to solve an M/M/c/m queueing system.

    Args:
        c: Number of active ovens (servers).
        m: Waiting queue capacity.
        lambda_rate: Order arrival rate.
        mu_rate: Service completion rate per oven.

    Returns:
        QueueingMetrics instance.
    """
    model = MMcmModel(c=c, m=m)
    return model.calculate_metrics(lambda_rate=lambda_rate, mu_rate=mu_rate)
