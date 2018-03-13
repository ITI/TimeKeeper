#include "module.h"
#include "utils.h"

/** EXTERN VARIABLES **/
extern int tracer_num; 					// number of TRACERS in the experiment
extern int n_processed_tracers;			// number of tracers for which a spinner has already been spawned
extern int EXP_CPUS;
extern int TOTAL_CPUS;
extern int experiment_stopped; 			 		// flag to determine state of the experiment
extern int experiment_status;
extern unsigned long orig_cr0; 
extern struct task_struct *loop_task;
extern struct task_struct * round_task;
					 		
extern struct mutex exp_lock;
extern int *per_cpu_chain_length;
extern llist * per_cpu_tracer_list;

extern hashmap poll_process_lookup;
extern hashmap select_process_lookup;
extern hashmap sleep_process_lookup;
extern hashmap get_tracer_by_id;			//hashmap of <TRACER_NUMBER, TRACER_STRUCT> 
extern hashmap get_tracer_by_pid;			//hashmap of <TRACER_PID, TRACER_STRUCT>
extern unsigned long orig_cr0; 
extern unsigned long **sys_call_table; 
extern atomic_t n_waiting_tracers;

/** REFERENCE SYSCALLS **/
extern asmlinkage long (*ref_sys_sleep)(struct timespec __user *rqtp, struct timespec __user *rmtp);
extern asmlinkage int (*ref_sys_poll)(struct pollfd __user * ufds, unsigned int nfds, int timeout_msecs);
extern asmlinkage int (*ref_sys_select)(int n, fd_set __user *inp, fd_set __user *outp, fd_set __user *exp, struct timeval __user *tvp);
extern asmlinkage long (*ref_sys_clock_nanosleep)(const clockid_t which_clock, int flags, const struct timespec __user * rqtp, struct timespec __user * rmtp);
extern asmlinkage int (*ref_sys_clock_gettime)(const clockid_t which_clock, struct timespec __user * tp);


/** HOOKED SYSCALLS **/
extern asmlinkage long sys_sleep_new(struct timespec __user *rqtp, struct timespec __user *rmtp);
extern asmlinkage int sys_poll_new(struct pollfd __user * ufds, unsigned int nfds, int timeout_msecs);
extern asmlinkage int sys_select_new(int n, fd_set __user *inp, fd_set __user *outp, fd_set __user *exp, struct timeval __user *tvp);
extern asmlinkage long sys_clock_nanosleep_new(const clockid_t which_clock, int flags, const struct timespec __user * rqtp, struct timespec __user * rmtp);
extern asmlinkage int sys_clock_gettime_new(const clockid_t which_clock, struct timespec __user * tp);

/** EXTERN FUNCTIONS **/
extern unsigned long **aquire_sys_call_table(void);



/** LOCALLY DEFINED GLOBAL VARIABLES **/
s64 boottime;
atomic_t is_boottime_set = ATOMIC_INIT(0);
atomic_t n_active_syscalls = ATOMIC_INIT(0);
atomic_t n_workers_running = ATOMIC_INIT(0);
atomic_t progress_n_rounds = ATOMIC_INIT(0);
atomic_t progress_n_enabled = ATOMIC_INIT(0);
atomic_t experiment_stopping = ATOMIC_INIT(0);

static wait_queue_head_t sync_worker_wqueue;
static wait_queue_head_t progress_call_proc_wqueue;
wait_queue_head_t progress_sync_proc_wqueue;
wait_queue_head_t expstop_call_proc_wqueue;
wait_queue_head_t* syscall_control_queue;

struct task_struct ** chaintask;
int* values;
int* syscall_running; 

int per_cpu_worker(void *data);
int round_sync_task(void *data);


/***
* Progress experiment for specified number of rounds
* write_buffer: <number of rounds>
***/
int progress_exp_fixed_rounds(char * write_buffer){

	int progress_rounds = 0;
	int ret = 0;


	if(experiment_stopped == NOTRUNNING)
		return FAIL;

	set_current_state(TASK_INTERRUPTIBLE);	
	progress_rounds = atoi(write_buffer);
	if(progress_rounds > 0 )
		atomic_set(&progress_n_rounds,progress_rounds);
	else
		atomic_set(&progress_n_rounds,1);	
		
	atomic_set(&progress_n_enabled,1);

	if(experiment_stopped == FROZEN){
		experiment_stopped = RUNNING;
		PDEBUG_A("progress exp n rounds: Waking up round_sync_task\n");
		while(wake_up_process(round_task) != 1);
		PDEBUG_A("progress exp n rounds: Woke up round_sync_task\n");
	}

	PDEBUG_V("Progress Exp For Fixed Rounds Initiated. Number of Progress rounds = %d\n", progress_rounds);
	wake_up_interruptible(&progress_sync_proc_wqueue);
	experiment_stopped = RUNNING;
	do{
			ret = wait_event_interruptible_timeout(progress_call_proc_wqueue,atomic_read(&progress_n_rounds) == 0,HZ);
			if(ret == 0)
				set_current_state(TASK_INTERRUPTIBLE);
			else
				set_current_state(TASK_RUNNING);

	}while(ret == 0);
	
	return SUCCESS;
}

/***
* Resume progressing without the constrain of progressing and stopping after a fixed number of rounds
***/
int resume_exp_progress(){

	if(experiment_stopped != RUNNING)
		return FAIL;

	if(atomic_read(&progress_n_enabled) == 1 && atomic_read(&progress_n_rounds) <= 0) {
		atomic_set(&progress_n_enabled,0);
		atomic_set(&progress_n_rounds,0);
		wake_up_interruptible(&progress_sync_proc_wqueue);
	}

	return SUCCESS;
}

/**
* Start Exp like resume_exp_progress. Should be called first before using any of progress_exp_fixed_rounds or resume_exp_progress
**/
void start_exp(){

	if(experiment_stopped != RUNNING){
		atomic_set(&progress_n_enabled,0);
		atomic_set(&progress_n_rounds,0);
		wake_up_interruptible(&progress_sync_proc_wqueue);
		experiment_stopped = RUNNING;
		PDEBUG_A("start exp: Waking up round_sync_task\n");
		while(wake_up_process(round_task) != 1);
		PDEBUG_A("start exp: Woke up round_sync_task\n");
	}

}




int initialize_experiment_components(){

	int i;

	PDEBUG_V("Entering Experiment Initialization\n");
	if(experiment_status == INITIALIZED){
		PDEBUG_E("Experiment Already initialized !\n");
		//cleanup_experiment_components();
		return FAIL;
	}


	per_cpu_chain_length = (int *) kmalloc(EXP_CPUS*sizeof(int), GFP_KERNEL);
	per_cpu_tracer_list = (llist *) kmalloc(EXP_CPUS*sizeof(llist), GFP_KERNEL);
	values = (int *)kmalloc(EXP_CPUS*sizeof(int), GFP_KERNEL);
	chaintask = kmalloc(EXP_CPUS*sizeof(struct task_struct*), GFP_KERNEL);
	syscall_running = (int *)kmalloc(EXP_CPUS*sizeof(int), GFP_KERNEL);
	syscall_control_queue = (wait_queue_head_t* )kmalloc(EXP_CPUS*sizeof(wait_queue_head_t), GFP_KERNEL);

	if(!per_cpu_tracer_list || !per_cpu_chain_length || !values || !chaintask || !syscall_running || !syscall_control_queue){
		PDEBUG_E("Error Allocating memory for per cpu structures.\n");
    	return -ENOMEM;
	}

	for(i = 0; i < EXP_CPUS; i++){
		llist_init(&per_cpu_tracer_list[i]);
		per_cpu_chain_length[i] = 0;
		init_waitqueue_head(&syscall_control_queue[i]);
	}
	
	mutex_init(&exp_lock);

	hmap_init( &poll_process_lookup,"int",0);
	hmap_init( &select_process_lookup,"int",0);
	hmap_init( &sleep_process_lookup,"int",0);
	hmap_init( &get_tracer_by_id,"int",0);
	hmap_init( &get_tracer_by_pid, "int", 0);

	init_waitqueue_head(&progress_call_proc_wqueue);
	init_waitqueue_head(&progress_sync_proc_wqueue);	
	init_waitqueue_head(&expstop_call_proc_wqueue);
	init_waitqueue_head(&sync_worker_wqueue);
	

	atomic_set(&progress_n_enabled,0);
	atomic_set(&progress_n_rounds,0);
	atomic_set(&experiment_stopping,0);
	atomic_set(&n_workers_running,0);
	atomic_set(&n_active_syscalls,0);
	atomic_set(&n_waiting_tracers,0);

	PDEBUG_V("Init experiment components: Initialized Variables\n");


	round_task = kthread_create(&round_sync_task, NULL, "round_sync_task");
	if(!IS_ERR(round_task)) {
	    wake_up_process(round_task);
	}
	else{
		PDEBUG_E("Error Starting Round Sync Task\n");
		return -EFAULT;
	}

	experiment_stopped = NOTRUNNING;
	experiment_status = INITIALIZED;


	/* Wait to stop loop_task */
	#ifdef __x86_64
	if (loop_task != NULL) {
    	kill(loop_task, SIGSTOP, NULL);
    	bitmap_zero((&loop_task->cpus_allowed)->bits, 8);
		cpumask_set_cpu(1,&loop_task->cpus_allowed);
		kill(loop_task, SIGCONT, NULL);
    }
	else {
        PDEBUG_E(" Loop_task is null\n");
    }
	#endif

	PDEBUG_V("Init experiment components: Finished\n");

	return SUCCESS;

}

int cleanup_experiment_components(){

	int i = 0;

	if(experiment_status == NOT_INITIALIZED){
		PDEBUG_E("Experiment Already Cleaned up ...\n");
		return FAIL;
	}

	PDEBUG_V("Cleaning up experiment components ...\n");

	hmap_destroy(&poll_process_lookup);
	hmap_destroy(&select_process_lookup);
	hmap_destroy(&sleep_process_lookup);
	hmap_destroy(&get_tracer_by_id);
	hmap_destroy(&get_tracer_by_pid);

	tracer_num = 0;
	n_processed_tracers = 0;
	//loop_task = NULL;
	round_task = NULL;

	atomic_set(&progress_n_enabled,0);
	atomic_set(&progress_n_rounds,0);
	atomic_set(&experiment_stopping,0);
	atomic_set(&n_workers_running,0);
	atomic_set(&n_active_syscalls,0);
	atomic_set(&n_waiting_tracers,0);



	if(sys_call_table){


		orig_cr0 = read_cr0();
		write_cr0(orig_cr0 & ~0x00010000);
		sys_call_table[__NR_nanosleep] = (unsigned long *)ref_sys_sleep;
		sys_call_table[__NR_clock_gettime] = (unsigned long *) ref_sys_clock_gettime;
		sys_call_table[__NR_clock_nanosleep] = (unsigned long *) ref_sys_clock_nanosleep;
		sys_call_table[__NR_poll] = (unsigned long *)ref_sys_poll;	
		sys_call_table[NR_select] = (unsigned long *)ref_sys_select;
		write_cr0(orig_cr0 | 0x00010000);

	}

	//if ( kthread_stop(round_sync_task) )
    //{
    //     PDEBUG_E("Clean up experiment components: Stopping round_sync_task error\n");
    //}

    for(i=0; i < EXP_CPUS; i++) {
    	llist_destroy(&per_cpu_tracer_list[i]);
    }
	
	kfree(syscall_running);
	kfree(syscall_control_queue);
	kfree(per_cpu_tracer_list);
	kfree(per_cpu_chain_length);
	kfree(values);
	kfree(chaintask);

	experiment_stopped = NOTRUNNING;
	experiment_status = NOT_INITIALIZED;

	return SUCCESS;


}

/***
*	write_buffer: <expected number of registered tracers>
**/ 
int sync_and_freeze(char * write_buffer) {

	int i;
	int j, n_expected_tracers;
	u32 flags;
	s64 now;
	struct timeval now_timeval;
	struct sched_param sp;
	tracer * curr_tracer;
	int ret;

	//ret = initialize_experiment_components();
	//if(ret != SUCCESS)
	//	return ret;

	if(experiment_status != INITIALIZED || experiment_stopped != NOTRUNNING)
		return FAIL;

	n_expected_tracers = atoi(write_buffer);

	PDEBUG_A("Sync And Freeze: ** Starting Experiment Synchronization **\n");

	if (tracer_num <= 0) {
		PDEBUG_E("Sync And Freeze: Nothing added to experiment, dropping out\n");
		return FAIL;
	}
	
	if(tracer_num != n_expected_tracers){
		PDEBUG_E("Sync And Freeze: Expected number of tracers: %d not present. Actual number of registered tracers: %d\n", n_expected_tracers, tracer_num);
		return FAIL;
	}

	if(n_processed_tracers != n_expected_tracers){
		PDEBUG_E("Sync And Freeze: Expected number of tracer spinner tasks: %d not present. Actual number of registered tracer spinners: %d\n", n_expected_tracers, n_processed_tracers);
		return FAIL;	
	}

	if (experiment_stopped != NOTRUNNING) {
        PDEBUG_A("Sync And Freeze: Trying to Sync Freeze when an experiment is already running!\n");
        return FAIL;
    }



	PDEBUG_A("Sync and Freeze: Hooking system calls\n");
	PDEBUG_A("Round Sync Task Pid = %d\n", round_task->pid);

	if(!sys_call_table){
		PDEBUG_E("Sync and freeze: syscall table not acquired !\n");
		return FAIL;
	}

	orig_cr0 = read_cr0();
	write_cr0(orig_cr0 & ~0x00010000);

	sys_call_table[NR_select] = (unsigned long *)sys_select_new;	
	sys_call_table[__NR_poll] = (unsigned long *) sys_poll_new;
	sys_call_table[__NR_nanosleep] = (unsigned long *)sys_sleep_new;
	sys_call_table[__NR_clock_gettime] = (unsigned long *) sys_clock_gettime_new;
	sys_call_table[__NR_clock_nanosleep] = (unsigned long *) sys_clock_nanosleep_new;

	write_cr0(orig_cr0 | 0x00010000 );


	for (j = 0; j < EXP_CPUS; j++) {
        values[j] = j;
	}

	for (i = 0; i < EXP_CPUS; i++)
	{
		PDEBUG_A("Sync And Freeze: Adding Worker Thread %d\n", i);	
		chaintask[i] = kthread_create(&per_cpu_worker, &values[i], "per_cpu_worker");
		if(!IS_ERR(chaintask[i])) {
            kthread_bind(chaintask[i],i % (TOTAL_CPUS - EXP_CPUS));
            wake_up_process(chaintask[i]);
            PDEBUG_A("Chain Task %d: Pid = %d\n", i, chaintask[i]->pid);
        }
	}


	do_gettimeofday(&now_timeval);
    now = timeval_to_ns(&now_timeval);

    for(i = 1; i <= tracer_num; i++){
    	curr_tracer = hmap_get_abs(&get_tracer_by_id, i);
    	if(curr_tracer){

    		PDEBUG_A("Sync And Freeze: Setting Virt time for Tracer %d and its children\n", i);	
    		set_children_time(curr_tracer, curr_tracer->tracer_task, now);
    	
	    	curr_tracer->round_start_virt_time = now;
	    	if(curr_tracer->spinner_task){
	    		curr_tracer->spinner_task->virt_start_time = now;
	    		curr_tracer->spinner_task->freeze_time = now;
	    		curr_tracer->spinner_task->past_physical_time = 0;
		        curr_tracer->spinner_task->past_virtual_time = 0;
				curr_tracer->spinner_task->wakeup_time = 0;

	    	}
    	}
    }

    experiment_stopped = FROZEN;

    //if its 64-bit, start the busy loop task to fix the weird bug
	#ifdef __x86_64
	kill(loop_task, SIGCONT);
	#endif

	PDEBUG_A("Finished Sync and Freeze\n");

	return SUCCESS;

}


/***
The function called by each synchronization thread (CBE specific). For every process it is in charge of
it will see how long it should run, then start running the process at the head of the chain.
***/
int per_cpu_worker(void *data)
{
	int round = 0;
	int cpuID = *((int *)data);
	tracer * curr_tracer;
	llist_elem * head;
	llist * tracer_list;
	s64 now;
	struct timeval ktv;
	ktime_t ktime;
	int run_cpu;

	set_current_state(TASK_INTERRUPTIBLE);

		
	PDEBUG_I("#### per_cpu_worker: Started per cpu worker Thread for Tracers alotted to CPU = %d\n",cpuID + 2);
	tracer_list =  &per_cpu_tracer_list[cpuID];

	
	/* if it is the very first round, don't try to do any work, just rest */
	if (round == 0){
		PDEBUG_I("#### per_cpu_worker: For Tracers alotted to CPU = %d. Waiting to be woken up !\n",cpuID + 2);
		goto startWork;
	}
	
	while (!kthread_should_stop())
	{
		
        if(experiment_stopped == STOPPING) {
        
            set_current_state(TASK_INTERRUPTIBLE);
		    atomic_dec(&n_workers_running);
		    run_cpu = get_cpu();   
			PDEBUG_V("#### per_cpu_worker: Stopping. Sending wake up from worker Thread for lxcs on CPU = %d. My Run cpu = %d\n",cpuID + 2,run_cpu);
		    wake_up_interruptible(&sync_worker_wqueue);
        	return 0;
        }

		head = tracer_list->head;
	    

		while(head != NULL){

			curr_tracer = (tracer *)head->item;

			if (schedule_list_size(curr_tracer) > 0) {
				PDEBUG_V("per_cpu_worker: Called  UnFreeze Proc Recurse on CPU: %d\n", cpuID +  2);					
				unfreeze_proc_exp_recurse(curr_tracer);
				PDEBUG_V("per_cpu_worker: Finished Unfreeze Proc on CPU: %d\n", cpuID + 2);
	           	
			}
			head = head->next;
		}
		PDEBUG_V("per_cpu_worker: Thread done with on %d\n",cpuID + 2);
		/* when the first task has started running, signal you are done working, and sleep */
		round++;
		set_current_state(TASK_INTERRUPTIBLE);
		atomic_dec(&n_workers_running);
		run_cpu = get_cpu();
		PDEBUG_V("#### per_cpu_worker: Sending wake up from per_cpu_worker on behalf of all Tracers on CPU = %d. My Run cpu = %d\n",cpuID + 2,run_cpu);
		wake_up_interruptible(&sync_worker_wqueue);
		

	startWork:
		schedule();
		set_current_state(TASK_RUNNING);
		run_cpu = get_cpu();
		PDEBUG_V("~~~~ per_cpu_worker: Woken up for Tracers on CPU =  %d. My Run cpu = %d\n",cpuID + 2,run_cpu);
		
	}
	return 0;
}

/***
The main synchronization thread (For CBE mode). When all tasks in a round have completed, this will get
woken up, increment the experiment virtual time,  and then wake up every other synchronization thread 
to have it do work
***/
int round_sync_task(void *data)
{
        int round_count = 0;
        struct timeval ktv;
        int i;
       
        struct timeval now;
	    s64 start_ns;
	    tracer * curr_tracer;
		int run_cpu;
				

	 	set_current_state(TASK_INTERRUPTIBLE);
	 	PDEBUG_I("round_sync_task: Started.\n");
                
		while (!kthread_should_stop())
        {

        	if(round_count == 0)
        		goto end;
            round_count++;
            redo:

            if (experiment_stopped == STOPPING)
            {

					PDEBUG_I("round_sync_task: Cleaning experiment via catchup task. Waiting for all cpu workers to exit...\n");
					set_current_state(TASK_INTERRUPTIBLE);
					atomic_set(&n_workers_running, EXP_CPUS);
					for (i=0; i < EXP_CPUS; i++) {
						/* chaintask refers to per_cpu_worker */	
						if(wake_up_process(chaintask[i]) == 1){ 
							PDEBUG_V("round_sync_task: Sync thread %d wake up\n",i);
						}
						else{
						    while(wake_up_process(chaintask[i]) != 1);
							PDEBUG_V("round_sync_task: Sync thread %d already running\n",i);
						}
					}
					wait_event_interruptible(sync_worker_wqueue, atomic_read(&n_workers_running) == 0);
					PDEBUG_I("round_sync_task: All cpu workers and all syscalls exited !\n");
                    clean_exp();
					return 0;
            }

            
				
			if(atomic_read(&experiment_stopping) == 1 && atomic_read(&n_active_syscalls) == 0){
				experiment_stopped = STOPPING;
				continue;
			}
			else if(atomic_read(&experiment_stopping) == 1) {
				PDEBUG_I("round_sync_task: Stopping. NActive syscalls = %d\n", atomic_read(&n_active_syscalls));
				atomic_set(&progress_n_rounds,0);
				atomic_set(&progress_n_enabled,0);

				set_current_state(TASK_INTERRUPTIBLE);
				for(i = 1; i <= tracer_num; i++){
					curr_tracer = hmap_get_abs(&get_tracer_by_id, i);
					if(curr_tracer){
						set_current_state(TASK_INTERRUPTIBLE);
						resume_all_syscall_blocked_processes(curr_tracer);
					}
				}

				PDEBUG_I("round_sync_task: Waiting for syscalls to exit\n");
				wait_event_interruptible(expstop_call_proc_wqueue, atomic_read(&n_active_syscalls) == 0);
				experiment_stopped = STOPPING;
				continue;
			}

			

			set_current_state(TASK_INTERRUPTIBLE);			
			run_cpu = get_cpu();
			PDEBUG_V("round_sync_task: Waiting for progress sync proc queue to resume. Run_cpu %d. N_waiting tracers = %d\n",run_cpu, atomic_read(&n_waiting_tracers));
			wait_event_interruptible(progress_sync_proc_wqueue, ((atomic_read(&progress_n_enabled) == 1 && atomic_read(&progress_n_rounds) > 0) || atomic_read(&progress_n_enabled) == 0) && atomic_read(&n_waiting_tracers) == tracer_num);
			if(atomic_read(&experiment_stopping) == 1){
				continue;
			}		
            do_gettimeofday(&ktv);

			/* wait up each synchronization worker thread, then wait til they are all done */
			if (EXP_CPUS > 0 && tracer_num  > 0) {

				PDEBUG_V("round_sync_task: Round Starting. Waking up worker threads\n");
				atomic_set(&n_workers_running, EXP_CPUS);
			
				for (i=0; i < EXP_CPUS; i++) {

					/* chaintask refers to per_cpu_worker */	
					if(wake_up_process(chaintask[i]) == 1){ 
						PDEBUG_V("round_sync_task: Sync thread %d wake up\n",i);
					}
					else{
					    while(wake_up_process(chaintask[i]) != 1){
					    	msleep(50);
					    }
						PDEBUG_V("round_sync_task: Sync thread %d already running\n",i);
					}
				}

				run_cpu = get_cpu();
				PDEBUG_V("round_sync_task: Waiting for per_cpu_workers to finish. Run_cpu %d\n",run_cpu);
            
                do_gettimeofday(&now);
                start_ns = timeval_to_ns(&now);


    			set_current_state(TASK_INTERRUPTIBLE);
				wait_event_interruptible(sync_worker_wqueue, atomic_read(&n_workers_running) == 0);
				

				PDEBUG_V("round_sync_task: All sync drift thread finished\n");	
				for(i = 0 ; i < EXP_CPUS; i++){
					 update_all_tracers_virtual_time(i);
				}
			}

			if(atomic_read(&progress_n_enabled) == 1 && atomic_read(&progress_n_rounds) > 0){
				atomic_dec(&progress_n_rounds);
				if(atomic_read(&progress_n_rounds) == 0) {
					PDEBUG_V("Waking up Progress rounds wait Process\n");
					wake_up_interruptible(&progress_call_proc_wqueue);
				}
			}

			/* if there are no continers in the experiment, then stop the experiment */
			/*if (tracer_num == 0 && experiment_stopped == RUNNING) {
				PDEBUG_I("round_sync_task: Cleaning experiment via catchup task because no tasks left\n");
           		clean_exp();	
				return 0;
			}*/
		    

			end:
                set_current_state(TASK_INTERRUPTIBLE);	
		
			if(experiment_stopped == NOTRUNNING){
				set_current_state(TASK_INTERRUPTIBLE);	
				round_count++;
				PDEBUG_I("round_sync_task: Waiting to be woken up\n");
				
				schedule();			
			}
			set_current_state(TASK_INTERRUPTIBLE);
			PDEBUG_V("round_sync_task: Resumed\n");
        }
        return 0;
}


void resume_all(tracer * curr_tracer, struct task_struct * aTask){

	struct list_head *list;
    struct task_struct *taskRecurse;
    struct task_struct *me;
    struct task_struct *t;

    struct poll_helper_struct * task_poll_helper = NULL;
	struct select_helper_struct * task_select_helper = NULL;
	struct sleep_helper_struct * task_sleep_helper = NULL;
	int cpu;

	if(curr_tracer && aTask && aTask != curr_tracer->tracer_task){

		cpu = curr_tracer->cpu_assignment - 2;
		me = aTask;
		t = me;
		do {


		    acquire_irq_lock(&t->dialation_lock,flags);
			task_poll_helper = hmap_get_abs(&poll_process_lookup,t->pid);
			task_select_helper = hmap_get_abs(&select_process_lookup,t->pid);
			task_sleep_helper = hmap_get_abs(&sleep_process_lookup, t->pid);

			if(task_poll_helper != NULL){
				
				t->freeze_time = 0;
				syscall_running[cpu] = 1;
				atomic_set(&task_poll_helper->done,1);
				
				PDEBUG_V("Poll Wakeup. Pid = %d\n", t->pid); 
				wake_up(&task_poll_helper->w_queue);
				release_irq_lock(&t->dialation_lock,flags);
				wait_event_interruptible(syscall_control_queue[cpu],syscall_running[cpu] == 0);
				PDEBUG_V("Poll Wakeup Resume. Pid = %d\n", t->pid); 
			}
			else if(task_select_helper != NULL){

				t->freeze_time = 0;
				syscall_running[cpu] = 1;
				atomic_set(&task_select_helper->done,1);
				
				PDEBUG_V("Select Wakeup. Pid = %d\n", t->pid); 
				wake_up(&task_select_helper->w_queue);
				release_irq_lock(&t->dialation_lock,flags);
				wait_event_interruptible(syscall_control_queue[cpu],syscall_running[cpu] == 0);
				PDEBUG_V("Select Wakeup Resume. Pid = %d\n", t->pid); 
			}
			else if( task_sleep_helper != NULL) {
			
				/* Sending a Continue signal here will wake all threads up. We dont want that */
				t->freeze_time = 0;
				syscall_running[cpu] = 1;
				atomic_set(&task_sleep_helper->done,1);
				
				PDEBUG_V("Sleep Wakeup. Pid = %d\n", t->pid); 
				wake_up(&task_sleep_helper->w_queue);
				release_irq_lock(&t->dialation_lock,flags);
				wait_event_interruptible(syscall_control_queue[cpu],syscall_running[cpu] == 0);
				PDEBUG_V("Sleep Wakeup Resume. Pid = %d\n", t->pid); 
			}
			else{
				release_irq_lock(&t->dialation_lock,flags);
			}
		}while_each_thread(me, t);

	}

	if(aTask && curr_tracer){
	    list_for_each(list, &aTask->children)
	    {
			taskRecurse = list_entry(list, struct task_struct, sibling);
	        if (taskRecurse->pid == 0) {
	            continue;
	        }
		    resume_all(curr_tracer,taskRecurse);
	    }
	
	}

}

void resume_all_syscall_blocked_processes(tracer * curr_tracer){

	if(curr_tracer)
		resume_all(curr_tracer, curr_tracer->tracer_task);

}

void clean_up_all_irrelevant_processes(tracer * curr_tracer){

	struct pid *pid_struct;
	struct task_struct *task;
	llist *schedule_queue ;
	llist_elem *head ;
	lxc_schedule_elem *curr_elem;
	int n_checked_processes = 0;
	int n_scheduled_processes = 0;
	

	if(!curr_tracer)
		return;

	curr_elem = NULL;

	//schedule_queue = &curr_tracer->schedule_queue;
	//head = schedule_queue->head;
	PDEBUG_V("Clean up irrelevant processes: Entered.\n");
	n_scheduled_processes = schedule_list_size(curr_tracer);
	PDEBUG_V("Clean up irrelevant processes: Entered. n_scheduled_processes: %d\n", n_scheduled_processes);

	while(n_checked_processes < n_scheduled_processes){

		//curr_elem = (lxc_schedule_elem *) schedule_list_get_head(curr_tracer);
		curr_elem = (lxc_schedule_elem *)llist_get(&curr_tracer->schedule_queue, 0);
		PDEBUG_V("Clean up irrelevant processes: Got head.\n");
		if(!curr_elem)
			return;	

		PDEBUG_V("Clean up irrelevant processes: Curr elem: %d. n_scheduled_processes: %d\n", curr_elem->pid, n_scheduled_processes);
		pid_struct = find_get_pid(curr_elem->pid); 	 
		task = pid_task(pid_struct,PIDTYPE_PID); 

		if(task == NULL || hmap_get_abs(&curr_tracer->ignored_children, curr_elem->pid) != NULL){
			
			if(task == NULL){ // task is dead
				//hmap_remove_abs(&curr_tracer->valid_children, curr_elem->pid);
				//hmap_remove_abs(&curr_tracer->ignored_children, curr_elem->pid);
				PDEBUG_V("Clean up irrelevant processes: Curr elem: %d. Task is dead\n", curr_elem->pid);
				pop_schedule_list(curr_tracer);
			}
			else{ // task is ignored
				PDEBUG_V("Clean up irrelevant processes: Curr elem: %d. Task is ignored\n", curr_elem->pid);
				hmap_remove_abs(&curr_tracer->valid_children, curr_elem->pid);
			}

			
		}
		else
			requeue_schedule_list(curr_tracer);
		n_checked_processes ++;		

	}
}


void update_all_runnable_task_timeslices(tracer * curr_tracer){
	llist* schedule_queue = &curr_tracer->schedule_queue;
	llist_elem* head = schedule_queue->head;
	lxc_schedule_elem* curr_elem;
	lxc_schedule_elem* tmp;

	struct poll_helper_struct * task_poll_helper = NULL;
	struct select_helper_struct * task_select_helper = NULL;
	struct sleep_helper_struct * task_sleep_helper = NULL;

	unsigned long flags;
	s64 total_insns = curr_tracer->quantum_n_insns;
	s64 n_alotted_insns = 0;
	int no_task_runnable = 1;

	#ifdef __TK_MULTI_CORE_MODE
	while(head != NULL) {
	#else
	while(head != NULL && n_alotted_insns < total_insns){
	#endif
		curr_elem = (lxc_schedule_elem *)head->item;

		if(!curr_elem){
			PDEBUG_V("Update all runnable task timeslices: Curr elem is NULL\n");
			return;
		}

		PDEBUG_V("Update all runnable task timeslices: Processing Curr elem Left\n");
		PDEBUG_V("Update all runnable task timeslices: Curr elem is %d. Quantum n_insns: %llu\n", curr_elem->pid, total_insns);

		acquire_irq_lock(&curr_elem->curr_task->dialation_lock,flags);
		task_poll_helper = hmap_get_abs(&poll_process_lookup,curr_elem->pid);
		task_select_helper = hmap_get_abs(&select_process_lookup,curr_elem->pid);
		task_sleep_helper = hmap_get_abs(&sleep_process_lookup, curr_elem->pid);

		if(task_poll_helper == NULL && task_select_helper == NULL && task_sleep_helper == NULL){

			no_task_runnable = 0;
			#ifdef __TK_MULTI_CORE_MODE
				curr_elem->n_insns_curr_round = total_insns;
				//n_alotted_insns = total_insns;
			#else
				if(curr_elem->n_insns_left > 0) { // there should exist only one element like this
					if(n_alotted_insns + curr_elem->n_insns_left > total_insns){
						curr_elem->n_insns_curr_round = total_insns - n_alotted_insns;
						curr_elem->n_insns_left = curr_elem->n_insns_left - curr_elem->n_insns_curr_round;
						n_alotted_insns = total_insns;
					}
					else{					
						curr_elem->n_insns_curr_round = curr_elem->n_insns_left;
						curr_elem->n_insns_left = 0;
						n_alotted_insns += curr_elem->n_insns_curr_round;						
					}

					curr_tracer->last_run = curr_elem;

				}

			#endif
		}
		else{
			curr_elem->n_insns_curr_round = 0;
			curr_elem->n_insns_left = 0;
		}

		release_irq_lock(&curr_elem->curr_task->dialation_lock,flags);
		head = head->next;
	}

	

	#ifndef __TK_MULTI_CORE_MODE
		if(n_alotted_insns < total_insns && no_task_runnable == 0){

			
			if(curr_tracer->last_run == NULL)
				head = schedule_queue->head;
			else{
				head = schedule_queue->head;
				while(head != NULL){
					tmp = (lxc_schedule_elem *)head->item;
					if(tmp == curr_tracer->last_run){
						head = head->next;
						break;
					}
					head = head->next;
				}

				if(head == NULL){
					//last run task no longer exists in schedule queue
					head = schedule_queue->head; //reset to head of schedule queue for now.
				}
			}

			while(n_alotted_insns < total_insns){


				curr_elem = (lxc_schedule_elem *)head->item;
				if(!curr_elem)
					return;

				PDEBUG_V("Update all runnable task timeslices: Processing Curr elem Share\n");
				PDEBUG_V("Update all runnable task timeslices: Curr elem is %d. Quantum n_insns: %llu\n", curr_elem->pid, total_insns);


				acquire_irq_lock(&curr_elem->curr_task->dialation_lock,flags);
				task_poll_helper = hmap_get_abs(&poll_process_lookup,curr_elem->pid);
				task_select_helper = hmap_get_abs(&select_process_lookup,curr_elem->pid);
				task_sleep_helper = hmap_get_abs(&sleep_process_lookup, curr_elem->pid);

				if(task_poll_helper == NULL && task_select_helper == NULL && task_sleep_helper == NULL){
					
						
						if(n_alotted_insns + curr_elem->n_insns_share > total_insns){
							curr_elem->n_insns_curr_round += total_insns - n_alotted_insns;
							curr_elem->n_insns_left = curr_elem->n_insns_share - (total_insns - n_alotted_insns); //for next round
							n_alotted_insns = total_insns;
						}
						else{
							curr_elem->n_insns_curr_round = curr_elem->n_insns_curr_round + curr_elem->n_insns_share;
							curr_elem->n_insns_left = 0;
							n_alotted_insns += curr_elem->n_insns_share;
						}

						if(n_alotted_insns == total_insns){
							curr_tracer->last_run = curr_elem;
							release_irq_lock(&curr_elem->curr_task->dialation_lock,flags);
							return;
						}
					
				}
				else{
					curr_elem->n_insns_curr_round = 0;
					curr_elem->n_insns_left = 0;
				}

				release_irq_lock(&curr_elem->curr_task->dialation_lock,flags);
				head = head->next;

				if(head == NULL){
					head = schedule_queue->head;
				}
			}

		}


	#endif
}



lxc_schedule_elem * get_next_runnable_task(tracer * curr_tracer){

	//llist * schedule_queue = &curr_tracer->schedule_queue;
	//llist_elem * head = schedule_queue->head;
	lxc_schedule_elem * curr_elem =NULL;
	int n_checked_processes = 0;
	int n_scheduled_processes = 0;
	

	if(!curr_tracer)
		return NULL;

	n_scheduled_processes = schedule_list_size(curr_tracer);

	while(n_checked_processes < n_scheduled_processes){

		curr_elem = (lxc_schedule_elem *)llist_get(&curr_tracer->schedule_queue, 0);

		if(!curr_elem)
			return NULL;	

		requeue_schedule_list(curr_tracer);
		n_checked_processes ++;
		if(curr_elem->n_insns_curr_round)
			return curr_elem;
		
			 
	}

	return NULL; // all processes are blocked. nothing to run.
	
}

void add_task_to_tracer_run_queue(tracer * curr_tracer, lxc_schedule_elem * elem){

	sprintf(curr_tracer->run_q_buffer + curr_tracer->buf_tail_ptr , "|%d,%d", elem->pid, elem->n_insns_curr_round);
	curr_tracer->buf_tail_ptr = strlen(curr_tracer->run_q_buffer);
}

void signal_cpu_worker_resume(tracer * curr_tracer){
	atomic_set(&curr_tracer->w_queue_control, 1);
	wake_up_interruptible(&curr_tracer->w_queue);

}

void signal_tracer_resume(tracer * curr_tracer){
	PDEBUG_V("Signal Tracer resume. Tracer : %d, Tracer ID: %d\n", curr_tracer->tracer_task->pid, curr_tracer->tracer_id);
	set_current_state(TASK_INTERRUPTIBLE);
	atomic_set(&curr_tracer->w_queue_control, 0);
	wake_up_interruptible(&curr_tracer->w_queue);
}

void wait_for_tracer_completion(tracer * curr_tracer){
	PDEBUG_V("Waiting for Tracer completion from Tracer : %d, Tracer ID: %d\n", curr_tracer->tracer_task->pid, curr_tracer->tracer_id);
	wait_event_interruptible(curr_tracer->w_queue, atomic_read(&curr_tracer->w_queue_control) == 1);
}

#ifndef __TK_MULTI_CORE_MODE
int unfreeze_proc_exp_single_core_mode(tracer * curr_tracer) {

	struct timeval now;
	s64 now_ns;
	s64 start_ns;
	int i = 0;
	unsigned long flags;
	struct poll_helper_struct * task_poll_helper = NULL;
	struct select_helper_struct * task_select_helper = NULL;
	struct sleep_helper_struct * task_sleep_helper = NULL;
	s64 rem_n_insns = 0;
	s64 total_insns = 0;
	lxc_schedule_elem * curr_elem;

	if(!curr_tracer)
		return FAIL;




	/* for adding any new tasks that might have been spawned */
	refresh_tracer_schedule_queue(curr_tracer);
	clean_up_all_irrelevant_processes(curr_tracer);
	update_all_runnable_task_timeslices(curr_tracer);
	flush_buffer(curr_tracer->run_q_buffer, BUF_MAX_SIZE);
	print_schedule_list(curr_tracer);
	curr_tracer->buf_tail_ptr = 0;

	total_insns = curr_tracer->quantum_n_insns;

	while(rem_n_insns < total_insns){
		curr_elem = get_next_runnable_task(curr_tracer);
		if(!curr_elem)
			break;
		add_task_to_tracer_run_queue(curr_tracer, curr_elem);
		if(rem_n_insns > 0)
			update_task_virtual_time(curr_tracer, curr_elem->curr_task,rem_n_insns);
		rem_n_insns += curr_elem->n_insns_curr_round;
		curr_elem->n_insns_curr_round = 0; // reset to zero
	}
	
	if(rem_n_insns >= total_insns){
		signal_tracer_resume(curr_tracer);
		wait_for_tracer_completion(curr_tracer);
	}
	
	resume_all_syscall_blocked_processes(curr_tracer);
	

	
	return SUCCESS;
}

#else
int unfreeze_proc_exp_multi_core_mode(tracer * curr_tracer) {

	struct timeval now;
	s64 now_ns;
	s64 start_ns;
	int i = 0;
	unsigned long flags;
	struct poll_helper_struct * task_poll_helper = NULL;
	struct select_helper_struct * task_select_helper = NULL;
	struct sleep_helper_struct * task_sleep_helper = NULL;
	int atleast_one_task_runnable = 0;
	

	if(!curr_tracer)
		return FAIL;


	llist * schedule_queue = &curr_tracer->schedule_queue;
	llist_elem * head = schedule_queue->head;
	lxc_schedule_elem * curr_elem;


	/* for adding any new tasks that might have been spawned */
	refresh_tracer_schedule_queue(curr_tracer);
	clean_up_all_irrelevant_processes(curr_tracer);
	update_all_runnable_task_timeslices(curr_tracer);
	flush_buffer(curr_tracer->run_q_buffer, BUF_MAX_SIZE);
	print_schedule_list(curr_tracer);
	curr_tracer->buf_tail_ptr = 0;

	
	while(head != NULL){
		curr_elem = (lxc_schedule_elem *)head->item;
		if(curr_elem && curr_elem->n_insns_curr_round > 0){
			add_task_to_tracer_run_queue(curr_tracer, curr_elem);
			atleast_one_task_runnable = 1;
			curr_elem->n_insns_curr_round = 0;
		}

		head = head->next;
	}
	
	if(atleast_one_task_runnable){
		signal_tracer_resume(curr_tracer);
		wait_for_tracer_completion(curr_tracer);
	}

	resume_all_syscall_blocked_processes(curr_tracer);

	return SUCCESS;
}
#endif

int unfreeze_proc_exp_recurse(tracer * curr_tracer){

	#ifdef __TK_MULTI_CORE_MODE
		return unfreeze_proc_exp_multi_core_mode(curr_tracer);
	#else
		return unfreeze_proc_exp_single_core_mode(curr_tracer);
	#endif
}	

void clean_exp(){

	int i;
	tracer * curr_tracer;

	set_current_state(TASK_INTERRUPTIBLE);
	wait_event_interruptible(progress_sync_proc_wqueue, atomic_read(&n_waiting_tracers) == tracer_num);
			
	PDEBUG_I("Clean exp: Cleaning up initiated ...");
	mutex_lock(&exp_lock);
	for(i = 1; i <= tracer_num; i++){


		curr_tracer = hmap_get_abs(&get_tracer_by_id, i);

		if(curr_tracer){
			clean_up_schedule_list(curr_tracer);
			flush_buffer(curr_tracer->run_q_buffer, BUF_MAX_SIZE);
			curr_tracer->buf_tail_ptr = 4;
			sprintf(curr_tracer->run_q_buffer,"STOP");
			atomic_set(&curr_tracer->w_queue_control,0);
			wake_up_interruptible(&curr_tracer->w_queue);
		}
	}
	mutex_unlock(&exp_lock);
	atomic_set(&experiment_stopping,0);
	wake_up_interruptible(&expstop_call_proc_wqueue);

}

