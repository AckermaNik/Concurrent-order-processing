# Concurrent Data Structures: Order Processing Simulation

This project is a C++20 concurrency exercise built around an order-processing workflow. Customer threads create orders, agent threads move them through a pending-order stack and a preparation queue, and cook threads record completed orders in per-customer ordered lists.

The workflow exercises three concurrent data structures with different synchronization strategies:

1. An atomic Treiber-style stack augmented with an elimination array and exponential backoff
2. A linked queue that uses separate locks for enqueue and dequeue
3. Sorted linked lists that use lock coupling for insertion and deletion

After the worker threads finish, the program checks whether the shared containers are empty and whether each customer's completed-order list has the expected size and checksum.

## Workflow overview

~~~text
Customer threads
  └─ create order IDs and push them
       ↓
PendingOrdersStack
  └─ atomic stack with elimination exchange
       ↓
Agent threads
  └─ pop orders and enqueue them for preparation
       ↓
UnderPreparationOrdersQueue
  └─ linked queue with separate head and tail locks
       ↓
Cook threads
  └─ dequeue orders and insert them into the matching customer list
       ↓
Districts[customer].completedOrders
  └─ sorted linked list using predecessor/current node locks
       ↓
Final size, sum, checksum, and emptiness checks
~~~

## Thread configuration

The Makefile sets N_THREADS=24 by default. If the Makefile does not define it, main.cpp defaults to N_THREADS=4.

The value must be positive and divisible by four. Compile-time static_assert checks enforce those requirements. The program divides the configured number into roles:

| Role | Count | Meaning |
| --- | ---: | --- |
| District/customer threads (DIST) | N_THREADS / 2 | Create orders and verify their completion |
| Agent threads (AGNT) | N_THREADS / 4 | Move orders from the stack to the queue |
| Cook threads (COOK) | N_THREADS / 4 | Move orders from the queue to customer lists |

With the default N_THREADS=24, this gives 12 customer threads, 6 agents, and 6 cooks. Each customer creates DIST orders, so this configuration processes 144 orders in total.

## Concurrent data structures

### Pending order stack

PendingOrdersStack stores newly created orders. Its head is an atomic pointer and is updated with compare-and-swap operations:

- tryPush() links a new node to the observed head, then attempts to publish it with compare_exchange_strong().
- tryPop() reads the head and attempts to replace it with the next node using compare_exchange_strong().
- push() and pop() retry failed operations and use an EliminationArray to let a concurrent push and pop exchange an order directly.
- Exponential backoff introduces a randomized delay after retries to reduce repeated contention on the shared stack head.

The elimination array has N_THREADS / 4 slots. A pushing thread publishes its order in a WAITING slot. A matching pop can claim the waiting value by changing the slot to BUSY, allowing the operations to complete without modifying the central stack.

PUSH_VALID is a special sentinel value used by a pop operation when it enters the elimination array. It distinguishes a pop request from a normal order ID. The project uses Order=int, so order values and sentinel values share the same type.

### Under-preparation queue

UnderPreparationOrdersQueue is a linked queue with a dummy sentinel node:

- Enqueue operations append at the tail while holding tail_lock.
- Dequeue operations advance the head while holding head_lock.
- The separate locks allow enqueue and dequeue work to proceed independently, subject to the linked-node structure.
- An atomic element counter tracks the queue size.

deq() waits until an item is available; the queue does not provide a blocking condition-variable interface.

### Completed-order lists

Each entry in Districts owns a CompletedOrdersList. Each list is sorted by order ID and has head and tail sentinel nodes.

Insertion and deletion use lock coupling:

1. Traverse to a predecessor (pred) and current (cur) node.
2. Lock the predecessor and current nodes.
3. Validate that neither node was deleted and that pred->next still points to cur.
4. Apply the insertion or deletion if the window is still valid.
5. Unlock both nodes; retry if validation failed.

searchL() traverses the ordered list without taking locks. In the simulation, a customer uses it to wait until each of its orders appears in its completed list before updating its checksum.

## Order IDs and validation

Customer thread tid creates the order IDs in the interval:

~~~text
tid * DIST  through  (tid + 1) * DIST - 1
~~~

A cook identifies the destination customer using integer division:

~~~text
customer ID = order ID / DIST
~~~

Once all threads have joined, main() reports:

- Whether the pending stack is empty
- Whether the preparation queue is empty
- The size of each completed-order list
- The sum of the IDs in each list
- Whether that sum matches the checksum accumulated by the customer thread

For each customer, the expected list size is DIST. The expected sum is computed from the customer's contiguous range of order IDs. These checks provide an end-to-end integrity check for the producer-to-agent-to-cook workflow.

## Source files

### main.cpp

Contains the complete project implementation:

- Thread-count constants and sentinel values
- ExponentialBackoff
- EliminationArray
- PendingOrdersStack
- UnderPreparationOrdersQueue
- Completed-order list and District operations
- Customer, agent, and cook thread routines
- Thread creation, joining, and final validation in main()

The implementation is currently kept in one source file rather than separated into headers and implementation files.

### Makefile

Build configuration for the program. It:

- Selects g++-10
- Compiles with C++20 and common warning flags
- Links pthread support
- Defines N_THREADS=24 by default
- Builds the executable named main
- Provides a clean target that removes the executable

Edit N_THREADS in this file to change the number of simulated threads. Use a positive value divisible by four.

### README.md

Project overview, design explanation, file guide, build instructions, and configuration notes.

### !!README

An informal developer note file. It describes an optional delay used to make stack exchanges easier to observe and mentions optional diagnostic printing for inspecting order delivery and list ordering.

### main

A prebuilt executable currently present in the project directory. It is generated output; rebuild it from main.cpp with the Makefile when changing the source or thread configuration.

### .vscode/

Editor and debugger configuration for Visual Studio Code. The launch configuration contains local absolute paths and may need adjustment on another machine.

## Build and run

The Makefile expects a compatible GNU C++ compiler, GNU Make, and pthread support. On this machine it names the compiler g++-10.

From the project directory:

~~~bash
make
./main
~~~

To remove the generated executable:

~~~bash
make clean
~~~

The executable takes no command-line arguments. It starts the configured worker threads, waits for them to finish, then prints PASS or FAIL validation lines.

## Useful debugging notes

The informal !!README file mentions an optional usleep(100000) delay near the stack push compare-and-swap. Uncommenting it can make elimination-array exchanges easier to observe, at the cost of slowing the run.

There are also commented diagnostic loops near the end of main.cpp for printing the completed orders and the items that passed through the preparation queue.

## Implementation notes and limitations

- The project demonstrates several synchronization approaches for coursework and experimentation; it is not presented as a production-ready container library.
- The queue's dequeue operation spins while the queue is empty.
- Customer threads also spin while waiting for their orders to appear in the completed-order list.
- The stack's elimination array uses timed polling and a shared pseudo-random generator.
- Thread-creation return codes and allocated thread arguments are not fully managed in the current implementation.
- The Makefile's compiler name (g++-10) is environment-specific; adjust CC if that command is unavailable.
- The current source combines the data structures, simulation, and checks in main.cpp; a future refactor could split those responsibilities into separate files.

## Project status

This is an academic concurrent-programming project. It demonstrates how different synchronization techniques can support a multithreaded workflow and includes end-to-end checks for order delivery and per-customer list contents.

