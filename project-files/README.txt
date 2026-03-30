team name: struct task_force
team member 1: NIKOLAOS TSOULOS sdi2300208
team member 2: DIMITRIOS ANDREAKIS sdi2300008
team member 3: DIMITRIOS ALEXANDRIS sdi2300002

--- Q1 ---

test1.c maps two memory regions (ptr1 and ptr2) and encodes a randomly generated message into the physical memory layout. 
In encode, it touches pages to allocate physical memory (setting bits to 1). 
In encode2, it unmaps specific pages (setting bits to 0). 
The decode function uses the user-space exposed page table (ept) to inspect the raw leaf PTE value for each virtual page of ptr. 
In our setup we interpret bit 0 of the leaf PTE as the present/valid indicator (x86_64 Present / arm64 PTE_VALID). 
If the bit is set, the corresponding page was mapped to a physical frame (bit 1 in the message); otherwise it is unmapped (bit 0). 
This reconstructs the message solely by looking at page table state.
If ept returns a non-zero value for a specific page index, it means a physical page exists (bit 1); otherwise, 
it is unmapped/zero (bit 0). This reconstructs the message solely by looking at page table state.


--- Q2 ---

Yes, see the code block for test1.c above. 
The decode logic calculates the EPT index via (virtual_addr >> 12) and checks the present/valid bit (ept[index] & 0x1).

--- Q3 ---

Because on an ept-patched kernel:
reading ept[vpn] reveals whether that virtual page has a present leaf PTE
encode() and encode2() reliably create present vs not-present patterns
decode() reads those PTEs via the exposed flat table -> reconstructs identical message bytes
So buf == buf2 == buf3 => prints success.	


--- Q4 ---

1. printf #1: first read of ept[...] definitely faults in the EPT VMA.
   The fault handler uses vmf->pgoff to select the PMD-sized chunk and walks the process page tables to the PMD level. 
   If the PMD does not point to a PTE-table page, it maps the global zero page into the EPT VMA; 
   otherwise it maps the real PTE-table page. The specific PTE entry for p is still empty, so the read prints <0x0>.

2. printf #2: same read again-now the EPT page is already mapped, so there is no EPT fault and it stays fast (still <0x0>).

3. write p[0]='C': this triggers a normal user page fault for p. The kernel allocates the data page and ensures there's a present PTE. 
   If this is the first time that PMD-sized chunk needs a PTE-table page, __pte_alloc()/pmd_install() runs and calls ept_invalidate(), 
   which zaps the corresponding EPT page mapping (and that zap path flushes).

4. printf #3:
   If ept_invalidate() zapped that EPT page, the next access refaults and remaps the real PTE-table page -> slow.
   If the PTE-table page already existed earlier (so no pmd_install() happened and no zap occurred), then the EPT 
   page already maps the real PTE-table page and you just read the updated entry -> fast.

5. printf #4: same as #3 but always fast as the EPT page is already mapped (no new faults).


--- Q5 ---

Across X=5/10/15/20 the consistent pattern is: #1 is slow (first EPT fault), #2 is fast, #4 is fast. 
The variability is #3, and that's why we included 2 back-to-back runs on test2.txt. Explanation below.

#3 is slow only if the write to p caused allocation/installation of a new PTE-table page for that PMD-sized chunk, because 
that triggers ept_invalidate() and therefore a second EPT fault at #3. 

#3 is fast when the PTE-table page already existed, so no zap/refault is needed. 

We verified this through testing, printing the address from pmd_install if current->comm is test2:

skidaim@skidaims-vm:~/userspace/hw3$ sudo ./test2
1. Time: 9708 (ns) -> <0x0>
2. Time: 161 (ns) -> <0x0>
address: 0x7a14858fd000
3. Time: 4749 (ns) -> <0x800000021d1a7867>
4. Time: 101 (ns) -> <0x800000021d1a7867>
skidaim@skidaims-vm:~/userspace/hw3$ sudo ./test2
1. Time: 12103 (ns) -> <0x0>
2. Time: 161 (ns) -> <0x0>
address: 0x7341d60a1000
3. Time: 60 (ns) -> <0x8000000222d6a867>
4. Time: 20 (ns) -> <0x8000000222d6a867>
skidaim@skidaims-vm:~/userspace/hw3$ sudo dmesg | grep 7a14858fd000
[  331.367907] EPT_DEBUG: pmd_install called for address 7a14858fd000
skidaim@skidaims-vm:~/userspace/hw3$ sudo dmesg | grep 7341d60a1000
skidaim@skidaims-vm:~/userspace/hw3$

Now, we have to mention that this seems to show up only at X=20 on our setup.
Our guess is that mmap()'s search uses the requested length, so changing X changes where p lands; 
At X=20 the allocation seems to more often lands in a PMD-sized region that didn't yet have a PTE-table page, 
making the zap -> refault at #3 case more likely. ASLR and current VMA layout make this non-deterministic run-to-run.
#3 is also slightly less worse than #1, probably due to caches.


--- GRADING ---
Overall, the non-bonus portion felt relatively straightforward once the page-table-walk and invalidation points were identified and 
implemented. The pmd_pfn tip felt a bit "spoon-fed", for lack of a better phrase. We believe our submission would strike in the ~95-100 
range on the core (non-bonus) requirements. We did not invest significant time into the optional bonus application beyond a small 
proof-of-concept demo. 


--- BONUS ---
The POC implements a tiny demo resident-set / "in-core working set" profiler for a user-space region. 
It allocates a large anonymous mapping, touches a configurable fraction of its pages, then repeatedly 
counts how many pages are resident using: (1) EPT reads of leaf PTE present bits via /dev/ept (no syscalls in the hot loop), 
and (2) the baseline mincore() syscall. Our /dev/ept mapping can generally serve as a fast replacement to mincore() (under some conditions),
that alone can produce a handful of POCs.

We measure average time per scan and compute scan throughput in Mpages/s. 
The point is to show that a user-exposed page table enables fast, frequent residency sampling without syscall overhead.

The minimal demo-y nature means a few limitations, like our calculation not being a true Denning "working set", mincore() and 
"present bit" not being perfectly identical semantics under all conditions, being architecture specific, etc. 