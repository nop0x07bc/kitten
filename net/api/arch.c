/** \file
 * Adapter layer for lwip to work with Kitten locks.
 */
#include <lwk/types.h>
#include <lwk/spinlock.h>
#include <lwk/task.h>
#include <lwk/aspace.h>
#include <lwk/sched.h>
#include <lwip/sys.h>


/** Semaphore and mailbox implementation for lwip */
spinlock_t sem_lock;
spinlock_t mbox_lock;

/** Debuging output toggle.
 * 0 == none
 * 1 == creation / deletion
 * 2 == mbox operations
 * 3 == semaphore operations
 */
static const int sem_debug = 0;

/** Rough estiamte of CPU speed in KHz */
static const uint64_t cpu_hz = 2400000;

void
sys_init( void )
{
	spin_lock_init( &sem_lock );
	spin_lock_init( &mbox_lock );
}


sys_sem_t
sys_sem_new(
	u8_t			count
)
{
	int * lock = kmem_alloc( sizeof( *lock ) );
	*lock = count;

	if( sem_debug >= 1 )
	printk( "%s: sem %p value %d\n", __func__, lock, count );

	return lock;
}


void
sys_sem_free(
	sys_sem_t		sem
)
{
	kmem_free( (void*) sem );
}


void
sys_sem_signal(
	sys_sem_t		sem
)
{
	unsigned long irqstate;
	spin_lock_irqsave( &sem_lock, irqstate );
	(*sem)++;
	spin_unlock_irqrestore( &sem_lock, irqstate );

	if( sem_debug >= 3 )
	printk( "%s: sem %p value %d\n", __func__, sem, *sem );
}


u32_t
sys_arch_sem_wait(
	sys_sem_t		sem,
	u32_t			timeout
)
{
	unsigned long irqstate;
	const uint64_t start_time = rdtsc();
	const uint64_t timeout_ticks = timeout * cpu_hz;

	if( sem_debug >= 3 )
	printk( "%s: waiting for sem %p value %d\n", __func__, sem, *sem );

	while( 1 )
	{
		const uint64_t delta_ticks = rdtsc() - start_time;
		if( timeout && delta_ticks > timeout_ticks )
			return SYS_ARCH_TIMEOUT;

		spin_lock_irqsave( &sem_lock, irqstate );
		const int value = *sem;

		if( !value )
		{
			// Not yet.  Try again
			spin_unlock_irqrestore( &sem_lock, irqstate );
			schedule();
			continue;
		}


		// We got it.
		(*sem)--;
		spin_unlock_irqrestore( &sem_lock, irqstate );

		if( sem_debug >= 3 )
		printk( "%s: sem %p value %d\n", __func__, sem, *sem );

		return delta_ticks / cpu_hz;
	}
}


sys_mbox_t
sys_mbox_new(
	int			size
)
{
	if( size <= 0 )
		size = 8;

	sys_mbox_t mbox = kmem_alloc( sizeof(*mbox) + sizeof(void*) * size );
	mbox->size = size;
	mbox->read = mbox->write = 0;

	if( sem_debug >= 1 )
	printk( "%s: %p @ %d\n", __func__, mbox, size );

	return mbox;
}


void
sys_mbox_free(
	sys_mbox_t		mbox
)
{
	if( mbox->read != mbox->write )
		printk( "%s: mbox has remaining elements %d/%d\n",
			__func__,
			mbox->read,
			mbox->write
		);

	kmem_free( mbox );
}



err_t
_sys_mbox_post(
	sys_mbox_t		mbox,
	void *			msg,
	int			try_once
)
{
	unsigned long irqstate;

	while(1)
	{
		spin_lock_irqsave( &mbox_lock, irqstate );
		const int write = mbox->write;
		const int next = (write + 1) % mbox->size;
		if( next != mbox->read )
		{
			mbox->msgs[ write ] = msg;
			mbox->write = next;
			spin_unlock_irqrestore( &mbox_lock, irqstate );

			if( sem_debug >= 2 )
			printk( "%s: mbox %p[%d] posting %p\n",
				__func__,
				mbox,
				write,
				msg
			);

			return ERR_OK;
		}

		spin_unlock_irqrestore( &mbox_lock, irqstate );

		if( try_once )
			return ERR_MEM;

		// Give up the CPU until we can check again
		schedule();
	}
}


void
sys_mbox_post(
	sys_mbox_t		mbox,
	void *			msg
)
{
	_sys_mbox_post( mbox, msg, 0 );
}


err_t
sys_mbox_trypost(
	sys_mbox_t		mbox,
	void *			msg
)
{
	return _sys_mbox_post( mbox, msg, 1 );
}


u32_t
sys_arch_mbox_tryfetch(
	sys_mbox_t		mbox,
	void **			msg
)
{
	unsigned long irqstate;
	spin_lock_irqsave( &mbox_lock, irqstate );
	const int read = mbox->read;
	if( read == mbox->write )
	{
		spin_unlock_irqrestore( &mbox_lock, irqstate );
		return SYS_MBOX_EMPTY;
	}

	*msg = mbox->msgs[ read ];
	mbox->read = ( read + 1 ) % mbox->size;
	spin_unlock_irqrestore( &mbox_lock, irqstate );

	if( sem_debug >= 2 )
	printk( "%s: mbox %p[%d] read %p\n",
		__func__,
		mbox,
		read,
		*msg
	);

	return 0;
}


u32_t
sys_arch_mbox_fetch(
	sys_mbox_t		mbox,
	void **			msg,
	u32_t			timeout
)
{
	const uint64_t start_time = rdtsc();
	const uint64_t timeout_ticks = timeout * cpu_hz;

	while( 1 )
	{
		const uint64_t delta_ticks = rdtsc() - start_time;
		if( timeout && delta_ticks > timeout_ticks )
		{
			if( sem_debug >= 3 )
			printk( "%s: timed out %d ticks %llx -> %llx\n", 
				__func__,
				timeout,
				start_time,
				rdtsc()
			);
			return SYS_ARCH_TIMEOUT;
		}

		if( sys_arch_mbox_tryfetch( mbox, msg ) == 0 )
			return delta_ticks / cpu_hz;

		// Put us to sleep for a bit
		schedule();
	}
}




/* Timeouts */
struct sys_timeouts timeouts[ 32 ];

struct sys_timeouts *
sys_arch_timeouts( void )
{
	return timeouts;
}


void (*kthread_entry)( void * arg );
void * kthread_arg;

static void
kthread_trampoline( void )
{
	int i;
	printk( "%s: new thread is running.  But where is my stack? %p\n", __func__, &i );

	kthread_entry( kthread_arg );

	printk( "%s: thread exited\n", __func__ );
	task_exit( 0 );
}


/* Threads */
sys_thread_t
sys_thread_new(
	char *			name,
	void 			(*thread)( void * arg ),
	void *			arg,
	int			stacksize,
	int			prio
)
{
	if( stacksize <= 0 )
		stacksize = 8192;

	uint8_t * stack = kmem_alloc( stacksize );
	printk( "%s: %s entry %p arg %p stack %d => %p\n",
		__func__,
		name,
		thread,
		arg,
		stacksize,
		stack
	);

	kthread_entry = thread;
	kthread_arg = arg;

	start_state_t	state = {
		.entry_point		= (vaddr_t) kthread_trampoline,
		.stack_ptr		= (vaddr_t) stack + stacksize,
		.aspace_id		= KERNEL_ASPACE_ID,
	};

	id_t id;
	task_create( ANY_ID, name, &state, &id );

	printk( "%s: New thread is id %d\n", __func__, id );
	return 0;
}
