#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "forth/forth_embed.h"

// if the region requested is already mapped, things fail
// so we want address that won't get used as the program
// starts up
#define STACKHEAP_MEM_START 0xf9f8c000

// the number of memory pages will will allocate to an instance of forth
#define NUM_PAGES 20

// the max number of pages we want in memort at once, ideally
#define MAX_PAGES 3

// 3 possible states of pages
#define ACTIVE 1
#define UN_MAPPED 2
#define SWAPPED 3

int priority[MAX_PAGES]; // store the priority of the MAX_PAGES pages that are mapped currently
int active[MAX_PAGES];	 // store the page_num of the MAX_PAGES active pages
int fd[NUM_PAGES];		 // store the file descriptors of all pages
int state[NUM_PAGES];	 // store the state of all pages
int num_active = 0;		 // tracks page count until the maximum is reached

static void handler(int sig, siginfo_t *si, void *unused)
{
	void *fault_address = si->si_addr;

	printf("in handler with invalid address %p\n", fault_address);

	// calculate the desired page number that caused this segfault
	int page_num = ((void *)fault_address - (void *)STACKHEAP_MEM_START) / getpagesize();
	if (page_num < 0 || page_num >= NUM_PAGES)
	{
		printf("address not within expected page!\n");
		exit(2);
	}

	void *desired_addr = (void *)STACKHEAP_MEM_START + (getpagesize() * page_num);

	// when we have already mapped the maximum number of pages
	// don't use num_active once it goes over MAX_PAGES
	int i;
	int active_idx = -1;

	if (num_active >= MAX_PAGES)
	{
		// find oldest page
		int cond = 1;
		while (cond)
		{
			for (int j = 0; j < MAX_PAGES; j++)
			{
				// set cond=0 when we find the oldest slot
				// store the index in active_idx
				// we should only find one page with pri=0, but break just in case
				if (priority[j] == 0)
				{
					active_idx = j;
					cond = 0;
					break;
				}
			}
			// decrease priorities
			for (int j = 0; j < MAX_PAGES; j++)
			{
				if (j != active_idx)
				{
					priority[j]--;
				}
			}
		}
		// this section will only run when we find the oldest page, stored at idx i
		i = active_idx;
		// write page's contents to a file

		
		void *addr = (active[i] * getpagesize()) + (void *)STACKHEAP_MEM_START;
		//write(fd[active[i]],addr, getpagesize());

		// unmap the page (evict)
		// index i still holds the index of the evicted page
		printf("unmapping page %d\n", active[i]);
		int munmap_result = munmap(addr, getpagesize());
		if (munmap_result < 0)
		{
			perror("munmap failed");
			exit(6);
		}
		close(fd[active[i]]);//close the old page 
		state[active[i]] = SWAPPED;
		//close(fd[active[i]]);
		// if the page has never been mapped before
		if (state[page_num] == UN_MAPPED)
		{
			// printf("mapping page %d\n", page_num);
			// void* result = mmap((void*) desired_addr,getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS,
			//  fd, 0);
			// if(result == MAP_FAILED) {
			// 	perror("map failed");
			// 	exit(1);
			// }
			// // set states for this particular page
			char filename[30];
			sprintf(filename, "page_%d.dat", page_num);
			fd[page_num] = open(filename, O_RDWR | O_CREAT, S_IRWXU);
			if (fd[page_num] < 0)
			{
				perror("error loading linked file");
				exit(25);
			}
			char data = '\0';
			lseek(fd, getpagesize() - 1, SEEK_SET);
			write(fd, &data, 1);
			lseek(fd, 0, SEEK_SET);

			char *result = mmap((void *)STACKHEAP_MEM_START,
								getpagesize(),
								PROT_READ | PROT_WRITE | PROT_EXEC,
								MAP_FIXED | MAP_SHARED,
								fd, 0);

			state[page_num] = ACTIVE;
			priority[i] = MAX_PAGES;
			active[i] = page_num;
		}
		// if the page is already on disk
		else if (state[page_num] == SWAPPED)
		{
			printf("mapping page %d\n", page_num);
			void *result = mmap((void *)desired_addr, getpagesize(), 
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_SHARED, 
			fd[page_num], 0);
			// set states for this page
			state[page_num] = ACTIVE;
			priority[i] = MAX_PAGES;
			active[i] = page_num;
		}
		else
		{
			printf("broke here\n");
			exit(5);
		}
	}
	else
	{
		printf("mapping page %d\n", page_num);
		void *result = mmap((void *)desired_addr, getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (result == MAP_FAILED)
		{
			perror("map failed");
			exit(1);
		}
		state[page_num] = ACTIVE;
		priority[num_active] = MAX_PAGES;
		active[num_active] = page_num;
		// decrease other priorities if they exist
		for (int j = 0; j < MAX_PAGES; j++)
		{
			if (j != num_active)
			{
				priority[j]--;
			}
		}
		num_active++;
	}
}
int main()
{

	//TODO: Add a bunch of segmentation fault handler setup here for
	//PART 1 (plus you'll also have to add the handler your self)
	// initialize global arrays
	for (int i = 0; i < NUM_PAGES; i++)
	{
		state[i] = UN_MAPPED;
		fd[i] = -1;
	}

	// installing SEGV signal handler
	// incidently we must configure signal handling to occur in its own stack
	// otherwise our segv handler will use the regular stack for its data
	// and it might try and unmap the very memory it is using as its stack
	static char stack[SIGSTKSZ];
	stack_t ss = {
		.ss_size = SIGSTKSZ,
		.ss_sp = stack,
	};

	sigaltstack(&ss, NULL);
	struct sigaction sa;

	// SIGINFO tells sigaction that the handler is expecting extra parameters
	// ONSTACK tells sigaction our signal handler should use the alternate stack
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = handler;
	if (sigaction(SIGSEGV, &sa, NULL) == -1)
	{
		perror("error installing handler");
		exit(3);
	}

	struct forth_data forth;
	char output[200];

	// the return stack is a forth-specific data structure if we
	// wanted to, we could give it an expanding memory segment like we
	// do for the stack/heap but I opted to keep things simple
	int returnstack_size = getpagesize() * 2;
	void *returnstack = mmap(NULL, returnstack_size, PROT_READ | PROT_WRITE | PROT_EXEC,
							 MAP_ANON | MAP_PRIVATE, -1, 0);

	// initializing the stack/heap to a unmapped memory pointer we
	// will map it by responding to segvs as the forth code attempts
	// to read/write memory in that space

	int stackheap_size = getpagesize() * NUM_PAGES;

	// TODO: Modify this in PART 1
	//void* stackheap = NULL; //mmap(NULL, stackheap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
	// MAP_ANON | MAP_PRIVATE, -1, 0);

	void *stackheap = (void *)STACKHEAP_MEM_START;
	initialize_forth_data(&forth,
						  returnstack + returnstack_size, //beginning of returnstack
						  stackheap,					  //begining of heap
						  stackheap + stackheap_size);	  //beginning of stack

	// this code actually executes a large amount of starter forth
	// code in jonesforth.f.  If you screwed something up about
	// memory, it's likely to fail here.
	load_starter_forth_at_path(&forth, "forth/jonesforth.f");

	printf("finished loading starter forth\n");

	// now we can set the input to our own little forth program
	// (as a string)
	int fresult = f_run(&forth,
						" : USESTACK BEGIN DUP 1- DUP 0= UNTIL ; " // function that puts numbers 0 to n on the stack
						" : DROPUNTIL BEGIN DUP ROT = UNTIL ; "	   // funtion that pulls numbers off the stack till it finds target
						" FOO 5000 USESTACK "					   // 5000 integers on the stack
						" 2500 DROPUNTIL "						   // pull half off
						" 1000 USESTACK "						   // then add some more back
						" 4999 DROPUNTIL "						   // pull all but 2 off
						" . . "									   // 4999 and 5000 should be the only ones remaining, print them out
						" .\" finished successfully \" "		   // print some text */
						,
						output,
						sizeof(output));

	if (fresult != FCONTINUE_INPUT_DONE)
	{
		printf("forth did not finish executing sucessfully %d\n", fresult);
		exit(4);
	}
	printf("OUTPUT: %s\n", output);
	printf("done\n");
	// close all of the files
	for (int i = 0; i < NUM_PAGES; i++)
	{
		close(fd[i]);
	}
	return 0;
}