# npu-perf-model MVP-4 Class Diagram and SystemC Module Connection Design

## 1. Module Hierarchy

    NPU_System

     |
     +-- DMA Engines
     |
     +-- Interconnect
     |
     +-- Arbiter
     |
     +-- MemoryController
     |
     +-- HBM

## 2. SystemC Module Types

  Module             Type
  ------------------ -----------
  NPU_System         sc_module
  DMA                sc_module
  Interconnect       sc_module
  MemoryController   sc_module
  HBM                sc_module
  Arbiter            C++ class

## 3. DMA

Responsibilities:

-   generate memory requests
-   send TLM transaction
-   wait response

Interface:

``` cpp
tlm_utils::simple_initiator_socket<DMA>
```

## 4. Interconnect

Interface:

``` cpp
class Interconnect : public sc_module
{
    target_socket dma_socket;
    initiator_socket memory_socket;

    Arbiter* arbiter;
};
```

Responsibilities:

-   request buffering
-   arbitration trigger
-   communication delay

## 5. Arbiter

Class hierarchy:

    Arbiter

     |
     +-- FIFOArbiter
     |
     +-- RoundRobinArbiter
     |
     +-- PriorityArbiter

Arbiter is not an sc_module because it has no independent timing.

## 6. MemoryController

Interface:

``` cpp
target_socket socket;
```

Responsibilities:

-   memory request queue
-   service timing
-   response generation

## 7. TLM Connection

    DMA
     |
    initiator socket
     |
    Interconnect
     |
    initiator socket
     |
    MemoryController
     |
    HBM

## 8. Transaction Flow

    DMA creates request

            |

    Interconnect receives

            |

    Request enters queue

            |

    Arbiter selects

            |

    MemoryController serves

            |

    HBM access

            |

    Response returned

## 9. Timing Ownership

  Module             Timing
  ------------------ -------------------------------
  DMA                request issue
  Interconnect       queue delay + network latency
  Arbiter            selection only
  MemoryController   scheduling delay
  HBM                memory latency

## 10. Development Order

Step 1: Request model

Step 2: FIFO Arbiter

Step 3: MemoryController

Step 4: Interconnect

Step 5: DMA migration

Step 6: Multi DMA test

Step 7: Round Robin/Priority

## 11. Final Architecture

                 NPU_System

                     |

            +----------------+

            | Interconnect   |

            +----------------+

                     |

                 Arbiter

                     |

            MemoryController

                     |

                    HBM
