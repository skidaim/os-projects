team name: struct task_force
team member 1: NIKOLAOS TSOULOS sdi2300208
team member 2: DIMITRIOS ANDREAKIS sdi2300008
team member 3: DIMITRIOS ALEXANDRIS sdi2300002

-Q1
A program whose process shows a high nivcsw is one that is frequently forcefully
preempted by the scheduler. It rarely waits on network/disk/user input and
almost always stays runnable. Because of that, it is preempted because its time
slice expires, rather than blocking or voluntarily yielding.

More specifically, this is a program we would usually call "CPU-intensive"
(often used synonymously with "computationally intensive"). Tight numeric
loops, encryption, compression, video encoding, back propagation for a neural network 
are typical examples. These processes keep the CPU busy until the scheduler has to 
step in and take the CPU away from them, which shows up as a high nivcsw count.

-Q2
A program with a high nvcsw frequently and voluntarily gives up the CPU before
its time slice is over because it cannot proceed until an external event
completes. It rarely stays runnable for long and often waits on
network/disk/user input or even on synchronization primitives.

This could be a database waiting for disk reads, a web server waiting for network requests,
or a text editor waiting for user keystrokes. These processes spend most of their time 
blocking in syscalls such as read(), voluntarily yielding the CPU while they wait.

-Q3
In our syscall, start_time comes from task_struct->start_time, which the kernel
uses to store the monotonic start time of the task in nanoseconds since boot.
From our observations, some processes (e.g. swapper/0 (PID 0), systemd (PID 1),
kthreadd (PID 2), many kworker/*, rcu_* threads, etc.) had start_time values
smaller than 200000000 nanoseconds (less than 0.2 seconds after boot). These
are obviously not user programs, but kernel threads or core system daemons that
are started automatically during system initialization.

Typical user-space applications have much larger start_time values. For
example, on our system gdm3 (the GNOME login manager) had start_time
10754328949 (about 10 seconds after boot), while VSCode (code-7d842fb85a) had
start_time 23548931163 (over 23 seconds after boot). This confirms that, obviously, 
user programs appear significantly later in the timeline than early kernel tasks.

Another interesting observation is that systemd and kthreadd (and over 30 pairs
of kernel worker threads, such as kworker/*) have identical start_time values
in nanoseconds. This makes sense because they are created in very tight
bursts during early boot. It also explains why, in our kernel implementation,
we needed a strict tiebreaker (PID) in addition to start_time to define a
deterministic and strict ordering among siblings in the DFS traversal.

-------------------------------------------------------
IMPORTANT NOTE: About sibling and child ordering in DFS
-------------------------------------------------------
We'd like to make a special note so as to not turn our comments into an essay.
In the first_child documentation, me mention:
"...we need a strict total order over all the independent thread-specific children lists"
One could argue that the "real" order of the children and siblings is well defined;
It is each thread's children list placed one after another (concatenated) in the
**order for_each_thread macro returns the threads**.
Example:
Suppose we have process P that consists of threads T1, T2, T3 as returned by for_each_thread. 
T1->children = (P1, P5)
T2->children = (P2, P3, P6)
T3->children = (P4)
Based on what we explained, the order of children of P would be:
(P1, P5, P2, P3, P6, P4)

We disagree with that notion. And it's not (just) because that means the resulted 
tree in test_k22tree output will pottentially have a list of siblings that are not
ordered by start_time (as implied by the examples). The problem statement is clear:

The assignment is asking that you expose information about processes, not threads.

We believe the correct interpretation for a process, at least in the spirit of this exercise, 
is an **unordered set of threads**. Why should we care about the order in which the kernel chooses to 
store these specific threads in memory (an implementation detail of the process abstraction), 
when trying to come up for an order of the CHILDREN of the process? That's why we define said order
as increasing start_time (could also be increasing PID based on the output examples).