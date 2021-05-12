
//written by Pavani Ravella and Zeyu Liao 
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "forth/forth_embed.h"
#include "forking_forth.h"

// if the region requested is already mapped, things fail
// so we want address that won't get used as the program
// starts up
#define UNIVERSAL_PAGE_START 0xf9f8c000

// the number of memory pages will will allocate to an instance of forth
#define NUM_PAGES 22 // last two pages are for the return stack
#define MAX_FORTHS 10

// this is a function I define for you - it's at the bottom of the
// file if you're curious
void push_onto_forth_stack(struct forth_data *data, int64_t value_to_push);
int fork_forth(int forknum);
void switch_current_to(int forth_num);
int find_available_slot();
static void handler(int sig, siginfo_t *si, void *unused);
#define PAGE_UNCREATED -1
char *frames; //mapped region that the forths will share
int forth_id; //global forth index
int frame_id = 0;

//create the forth extra data structure 
struct forth_extra_data
{
    int page_table[NUM_PAGES];
    bool valid;
    struct forth_data data;
};

struct forth_extra_data forth_extra_data[MAX_FORTHS]; //create an array forth esxtra data structures
int frames_fd; //keep track of the current frame index; 
int used_pages_count=-1; //keep track of the used pages 
int num_shared[NUM_PAGES * MAX_FORTHS]; //keep track of shared frames

//get the used paged counts 
int get_used_pages_count()
{
    if(used_pages_count ==-1) return 0;
    return used_pages_count;
}

bool first_time = true;


//from last homework of grapb seg fault
static void handler(int sig, siginfo_t *si, void *unused)
{
    //STEP 1-2 
    void *fault_address = si->si_addr;
    // int i=0;
    // int index =-1;
    // printf("in handler with invalid address %p\n", fault_address);
    int distance = ((void *)fault_address - (void *)UNIVERSAL_PAGE_START) / getpagesize();
    if (distance < 0 || distance > NUM_PAGES)
    {
        printf("address not within expected page!\n");
        exit(2);
    }
    int num = forth_extra_data[forth_id].page_table[distance];

    if (num >=1)
    {
        void *datap = (void *)frames + (num * getpagesize());
        void *target_addr = (void *)frames + (frame_id * getpagesize());

        // munmap the page in universal array first
        int munmap_result = munmap((void *)UNIVERSAL_PAGE_START + (distance * getpagesize()), getpagesize());
        if (munmap_result < 0)
        {
            perror("munmap failed");
            exit(6);
        }
        //do the mem copy to get the target address 
        void *mem_result = memcpy(target_addr, datap, getpagesize());
        if (mem_result != target_addr)
        {
            perror("memcpy failed");
            exit(4);
        }

        //caculate the target address to the mmap 
        target_addr = (void *)UNIVERSAL_PAGE_START + (getpagesize() * distance);
        mem_result = mmap(target_addr, getpagesize(),
                          PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_SHARED | MAP_FIXED,
                          frames_fd,
                          frame_id * getpagesize());
        if (mem_result == MAP_FAILED)
        {
            perror("map failed");
            exit(1);
        }

        forth_extra_data[forth_id].page_table[distance] = frame_id;
        frame_id++;
        num_shared[num]--;
        used_pages_count++;
    }
    else
    {
        void *result2 = mmap((void *)(void *)UNIVERSAL_PAGE_START + (getpagesize() * distance),
                             getpagesize(),
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_SHARED | MAP_FIXED,
                             frames_fd,
                             frame_id * getpagesize());
        if (result2 == MAP_FAILED)
        {
            perror("map failed");
            exit(1);
        }
        forth_extra_data[forth_id].page_table[distance] = frame_id;
        used_pages_count++;
        frame_id++;
    }


}

//creating the forths and intializing them and making the basic arrays 

void initialize_forths()
{
    if (first_time)
    {
        for (int i = 0; i < MAX_FORTHS * NUM_PAGES; i++)
        {
            num_shared[i] = 1;
        }
        // here's the place for code you only want to run once, like registering
        // our SEGV signal handler
        frames_fd = open("bigmem.dat", O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        if (frames_fd < 0)
        {
            perror("error loading linked file");
            exit(25);
        }
        char data = '\0';
        lseek(frames_fd, getpagesize() * NUM_PAGES * MAX_FORTHS, SEEK_SET);
        write(frames_fd, &data, 1);
        lseek(frames_fd, 0, SEEK_SET);

        frames = mmap(NULL, getpagesize() * NUM_PAGES * MAX_FORTHS,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_SHARED, frames_fd, 0);
        if (frames == NULL)
        {
            perror("frame map failed");
            exit(1);
        }
        //Grab from last homework's segfault_catch_example.c
        static char stack[SIGSTKSZ];
        stack_t ss = {
            .ss_size = SIGSTKSZ,
            .ss_sp = stack,
        };

        sigaltstack(&ss, NULL);

        struct sigaction sa;

        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = handler;
        if (sigaction(SIGSEGV, &sa, NULL) == -1)
        {
            perror("error installing handler");
            exit(3);
        }

        first_time = false;
    }
    // here's the place for code you want to run every time we run a test case

    // mark all the forths as invalid
    for (int i = 0; i < MAX_FORTHS; i++)
    {
        forth_extra_data[i].valid = false;
    }

    used_pages_count = 0;
}

//this function switches universla region to the given forth instance
void switch_current_to(int forth_num)
{
    forth_id = forth_num;
    //mmap the universal array
    int munmap_result = munmap((void *)UNIVERSAL_PAGE_START, getpagesize() * NUM_PAGES);
    if (munmap_result < 0)
    {
        perror("munmap failed");
        exit(6);
    }

    for (int i = 0; i < NUM_PAGES; i++)
    {
        if (forth_extra_data[forth_id].page_table[i] != -1)
        {
            int frame_num = forth_extra_data[forth_id].page_table[i];
            void *desired_addr = (void *)UNIVERSAL_PAGE_START + (getpagesize() * i);
            void *result;
            //if the frame is shared mark as read only

            if (num_shared[frame_num] > 1)
            {   
                //it is shared
                result = mmap(desired_addr, getpagesize(), PROT_READ | PROT_EXEC,
                              MAP_SHARED | MAP_FIXED,
                              frames_fd,
                              frame_num * getpagesize());
                if (result == MAP_FAILED)
                {
                    perror("result1 map fail");
                    exit(1);
                }
            }
            //if this frame is not shared, mark as write and read 
            else
            {
                result = mmap(desired_addr,
                              getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_SHARED | MAP_FIXED,
                              frames_fd,
                              frame_num * getpagesize());
                if (result == MAP_FAILED)
                {
                    perror("result2 map fail");
                    exit(1);
                }
            }

            // if(result == MAP_FAILED){
            //     perror("result map fail");
            //     exit(1);
            // }
        }
    }
    //STEP 0-1
    // int count = forth_num*getpagesize()*NUM_PAGES;
    // void* result = mmap((void*) UNIVERSAL_PAGE_START,
    // 				getpagesize()*NUM_PAGES,
    // 				PROT_READ | PROT_WRITE | PROT_EXEC,
    // 				MAP_SHARED | MAP_FIXED,
    // 				frames_fd,
    // 				count);
    // if(result == MAP_FAILED) {
    // 	perror("map failed");
    // 	exit(1);
    // }
}
int find_available_slot()
{
    int forth_num;
    for (forth_num = 0; forth_num < MAX_FORTHS; forth_num++)
    {
        if (forth_extra_data[forth_num].valid == false)
        {//founded
            break; 
        }
    }
    if (forth_num == MAX_FORTHS)
    {
        printf("We've created too many forths!");
        exit(1);
    }
    return forth_num;
}

// This function creates a brand new forth instance (not a fork) with the given code
// The function returns the id num of the newly created forth
int create_forth(char *code)
{
    int forth_num = find_available_slot();
    forth_extra_data[forth_num].valid = true;

    for (int i = 0; i < NUM_PAGES; i++)
    {
        forth_extra_data[forth_num].page_table[i] = PAGE_UNCREATED;
    }
    // STEP 0
    // this is where you should allocate NUM_PAGES*getpagesize() bytes
    // starting at position UNIVERSAL_PAGE_START to get started
    //
    // use mmap
    // void* result = mmap((void*) UNIVERSAL_PAGE_START,
    //                     NUM_PAGES*getpagesize(),
    //                     PROT_READ | PROT_WRITE | PROT_EXEC,
    //                     MAP_FIXED | MAP_SHARED| MAP_ANONYMOUS,
    //                     -1, 0);
    // if(result == MAP_FAILED) {
    //     perror("map failed");
    //     exit(1);
    // }
    switch_current_to(forth_num);

    // the return stack is a forth-specific data structure.  I
    // allocate a seperate space for it as the last 2 pages of
    // NUM_PAGES.
    int returnstack_size = getpagesize() * 2;

    int stackheap_size = getpagesize() * (NUM_PAGES - 2);

    // note that in this system, to make forking possible, all forths
    // are created with pointers only in the universal memory region
    initialize_forth_data(&forth_extra_data[forth_num].data,
                          (void *)UNIVERSAL_PAGE_START + stackheap_size + returnstack_size, //beginning of returnstack
                          (void *)UNIVERSAL_PAGE_START,                                     //begining of heap
                          (void *)UNIVERSAL_PAGE_START + stackheap_size);                   //beginning of the stack

    load_starter_forth_at_path(&forth_extra_data[forth_num].data, "forth/jonesforth.f");

    char output[100], input[100];

    // creating the fork function using FCONTINUE_FORK so we don't have to hard-code its value
    snprintf(input, 100, ": FORK %d PAUSE_WITH_CODE ;", FCONTINUE_FORK);

    // add a super tiny bit of forth which adds the FORK function
    f_run(&forth_extra_data[forth_num].data, input, output, 100);

    forth_extra_data[forth_num].data.input_current = code;
    
    return forth_num;
}

struct run_output run_forth_until_event(int forth_to_run)
{
    struct run_output output;
    switch_current_to(forth_to_run);
    output.result_code = f_run(&forth_extra_data[forth_to_run].data,
                               NULL,
                               output.output,
                               sizeof(output.output));
    output.forked_child_id = -1; // this should only be set to a value if we are forking
    if (output.result_code == FCONTINUE_FORK)
    {
        // printf("fork is implemented\n");
        output.forked_child_id = fork_forth(forth_to_run);
        // used_pages_count++;
    }
    return output;
}

//STEP 3 fork_forth
int fork_forth(int forknum)
{  
    int parent = forknum;
    int child_id = find_available_slot();
    forth_extra_data[child_id].valid = true;

    // copy from parent
    void *result = memcpy(&forth_extra_data[child_id].data,
                          &forth_extra_data[parent].data,
                          sizeof(struct forth_data));
    if (result != &forth_extra_data[child_id].data)
    {
        perror("memcpy failed");
        exit(4);
    }

    for (int i = 0; i < NUM_PAGES; i++)
    {
        if (forth_extra_data[parent].page_table[i] != -1)
        {
            int frame = forth_extra_data[parent].page_table[i];
            forth_extra_data[child_id].page_table[i] = frame;
            num_shared[frame]++;
            //used_pages_count++;
        }
    }

    // push 0 on child forth stack
    switch_current_to(child_id);
    push_onto_forth_stack(&forth_extra_data[child_id].data, 0);

    // push 1 on parent forth stack
    switch_current_to(parent);
    push_onto_forth_stack(&forth_extra_data[parent].data, 1);
    return child_id;
}

void push_onto_forth_stack(struct forth_data *data, int64_t value_to_push)
{
    int64_t current_top = *((int32_t *)data->stack_top);
    *((int64_t *)data->stack_top) = value_to_push;
    data->stack_top -= 8; // stack is 8 bytes a entry, starts high,
                          // goes low
    *((int64_t *)data->stack_top) = current_top;
}