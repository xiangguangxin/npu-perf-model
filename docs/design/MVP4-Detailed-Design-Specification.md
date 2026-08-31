# npu-perf-model MVP-4 Detailed Design Specification v1.1

## 1. Goal

MVP-4 upgrades MVP-3 from a single DMA-memory path into a
contention-aware NPU communication model.

Main additions:

-   Interconnect
-   Arbiter
-   Memory Controller
-   Queueing delay
-   Backpressure
-   Multi-request contention

## 2. Architecture Scope

MVP-4 models:

-   Single-hop shared interconnect
-   Multiple request sources
-   Arbitration
-   Queueing
-   Memory bandwidth limitation

Not included:

-   Multi-hop NoC
-   Router pipeline
-   Routing algorithm
-   Cache coherence

## 3. Hardware Model

    DMA0
     |
    DMA1
     |
    Interconnect
     |
    Arbiter
     |
    Memory Controller
     |
    HBM

## 4. Modules

### Request

MemoryRequest records:

-   request id
-   address
-   size
-   source
-   issue time
-   grant time
-   finish time
-   state

States:

    CREATED
     |
    QUEUED
     |
    GRANTED
     |
    SERVING
     |
    COMPLETED

## 5. Interconnect

Responsibilities:

-   receive requests
-   maintain request queue
-   add communication latency
-   forward requests to arbiter

Parameters:

``` cpp
queue_depth
bandwidth
latency_cycle
```

## 6. Arbiter

Supported policies:

### FIFO

First come first serve.

### Round Robin

Fair scheduling.

### Priority

QoS based scheduling.

Metrics:

-   latency
-   throughput
-   fairness
-   starvation

## 7. Memory Controller

The memory controller is inserted between arbiter and HBM.

Functions:

-   request queue
-   scheduling
-   bandwidth calculation
-   latency generation

## 8. Timing Model

    Ttotal =
    Tqueue +
    Tinterconnect +
    Tmemory_controller +
    THBM

Bandwidth:

    BW = min(
    Interconnect BW,
    Memory Controller BW,
    HBM BW
    )

## 9. Queue and Backpressure

Each queue has finite depth.

When queue is full:

-   DMA stalls
-   request waits

Statistics:

-   stall cycles
-   queue full events

## 10. System Parameters

Example:

    --dma 4
    --arbiter fifo
    --noc-latency 2

## 11. Verification

Tests:

1.  MVP-3 compatibility
2.  Two DMA contention
3.  Queue overflow
4.  Arbiter fairness

## 12. Experiments

DMA scaling:

    1/2/4/8 DMA

Arbiter comparison:

    FIFO
    Round Robin
    Priority

HBM bandwidth:

    64/128/256/512 GB/s

## 13. Acceptance Criteria

Architecture:

-   Interconnect implemented
-   Arbiter implemented
-   Memory Controller implemented

Functionality:

-   Multi-request support
-   Contention modeling
-   Backpressure

Research:

-   Performance evaluation
-   Fairness analysis
