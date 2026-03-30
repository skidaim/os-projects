team name: struct task_force
team member 1: NIKOLAOS TSOULOS sdi2300208
team member 2: DIMITRIOS ANDREAKIS sdi2300008
team member 3: DIMITRIOS ALEXANDRIS sdi2300002

RESULTS OF THE MICROBENCHMARK

========== GRR DEFAULT GROUP CPUS: 2 | GRR PERFORMANCE GROUP CPUS: 6 ==========
Daemons are running on cpus: 0, 1, 0, 1, 1, 1
========== DEFAULT ==========
Time: 26.764s
Time: 26.724s
Time: 28.862s
Time: 30.510s
========== PERFORMANCE ==========
Time: 6.280s
Time: 6.283s
Time: 6.301s
Time: 6.328s

========== GRR DEFAULT GROUP CPUS: 4 | GRR PERFORMANCE GROUP CPUS: 4 ==========
Daemons are running on cpus: 1, 2, 0, 3, 1, 3
========== DEFAULT ==========
Time: 13.715s
Time: 14.743s
Time: 15.139s
Time: 15.547s
========== PERFORMANCE ==========
Time: 6.523s
Time: 6.527s
Time: 6.618s
Time: 6.619s

========== GRR DEFAULT GROUP CPUS: 6 | GRR PERFORMANCE GROUP CPUS: 2 ==========
Daemons are running on cpus: 2, 0, 4, 3, 1, 2
========== DEFAULT ==========
Time: 11.851s
Time: 12.063s
Time: 12.146s
Time: 12.339s
========== PERFORMANCE ==========
Time: 13.285s
Time: 13.285s
Time: 13.269s
Time: 13.270s

Q1:

Yes.
In heavy.c, the main thread blocks all worker threads using a condition variable (pthread_cond_wait) 
until the syscall assign_process_to_group is completed. The output not only shows the clear performance 
edge GRR_PERFORMANCE tasks have, which indicates correct migration to the multiple cpu cores, but also shows 
consistent execution times for all threads within a specific run (e.g., all threads in the "GRR_PERFORMANCE" 
block of the first run finish in ~6.3ss). 

Q2:

Yes.
There is a direct correlation between the number of cores assigned and the completion time.

-Run 1 (2 Default / 6 Perf): 
GRR_DEFAULT is slow (~27s) because it is squeezed onto 2 cores which also fit (almost) every other 
task in the system. GRR_PERFORMANCE is fast (~6.3s) because it enjoys 6 cores, which are (almost) empty.

-Run 2 (4 Default / 4 Perf): 
GRR_DEFAULT speeds up significantly (~14s) as it gains cores. 
GRR_PERFORMANCE doesn't really take a hit (~6.3s -> ~6.6s) as it loses cores.
It's trivial to see why even though the 2 groups have the same amount of cores, they don't perform the same.
GRR_DEFAULT cores also have almost every other task in the system , and we still have as many (almost) 
empty cores as we have GRR_PERFORMANCE tasks. GRR_PERFORMANCE tasks still take the edge.

-Run 3 (6 Default / 2 Perf): 
Default speeds up again (~12s). 
Performance slows drastically and (as expected) doubles to ~13.3s as it is crushed into just 2 cores. 
This dynamic shift proves the syscall successfully re-partitioned the CPUs and forced tasks to migrate 
to valid cores. Here, we see that even though GRR_PERFORMANCE has 2 empty cores all to itself, 6 cores 
is just too much to compete with, even with the contention of all the other running system tasks.

Q3:

Yes. We explained this in more detail in Q2, the GRR_PERFORMANCE group outperforms GRR_DEFAULT 
up to the point where we give GRR_DEFAULT too many cores.

Q4:

Yes. 

2 Default Cores: Daemons are packed onto CPUs 0, 1.

4 Default Cores: Daemons spread to CPUs 0, 1, 2, 3.

6 Default Cores: Daemons spread further, appearing on CPUs 0, 1, 2, 3, 4.

This confirms that your scheduler class correctly load balanced the daemon processes.
It could also be the .balance callback stealing daemon tasks and pushing them onto the 
newly idle CPUs (since we migrated everything off of them).


--- GRADING ---
We believe this submission merits a grade of above 90. That is because correct and functional scheduler, 
syscalls, and load balancing accounts ALREADY for 90%, and even though one could argue the definition of a 
"correct" scheduling class, which "works alongside" the  existing Linux scheduler classes and "correctly implements" 
the 2 syscalls and load balancing, I think our implementation is pretty on par. 

We put much effort to extend our code beyond simply meeting the functional requirements and addresses the deeper 
synchronization challenges inherent in kernel development.The truth is,  the last 1-2 weeks before the assignment deadline, 
almost no line of code was altered in grr.c. Instead we focused heavily on ensuring robustness in edge cases when it comes to 2 syscalls. 
This is we admit in taking a small hit here. Some time COULD be devoted to areas that could be more "upstream-polished", 
instead of trying to reinvent migration.

Still, we tried stress tests for fairness, partitioning, load balancing, correct migration, and wanted to minimize the 
windows for a potential system crash or group-specific affinity violation. But even without all that, provided we account for
the scope of what most groups of... normal students at a university OS course could generally and reasonably produce, we can 
confidently strike for a 95-100.