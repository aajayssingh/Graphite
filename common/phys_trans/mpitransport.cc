#include "mpitransport.h"

//#define PT_DEBUG 1

// Initialize class static variables (these are useless but required by C++)
int Transport::pt_num_mod = 0;
int* Transport::dest_ranks = NULL;
UInt32 Transport::MCP_tag = 32767;

// This routine should be executed once in each process
void Transport::ptInitQueue(int num_mod)
{
   UINT32 i, j;
   UINT32 num_procs;    // Number of MPI processes
   UINT32 num_cores;
   int temp;
   int* thread_counts, *displs, *rbuf;
  
   // num_mod is the number of threads in this process
   pt_num_mod = num_mod;
  
   int provided;
   MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
   assert(provided >= MPI_THREAD_MULTIPLE);      

   // Determine the tag to use for communication with the MCP by asking
   //  for the maximum allowable tag value (tags must be between 0 and
   //  this upper bound, inclusive).
   int flag;
   int* max_tag;
   MPI_Comm_get_attr(MPI_COMM_WORLD, MPI_TAG_UB, &max_tag, &flag);
   // flag==false means that the attribute was not set
   assert(flag == true);
   MCP_tag = *max_tag;

   // FIXME?: We should probably just read the entire map of cores to
   //  processes from the config and skip all this swapping of core IDs.
   /* Quick hack
   MPI_Comm_rank(MPI_COMM_WORLD, &temp);
   my_threads = new int[num_mod];
   for (i=0; i < (UINT32)num_mod; i++) { my_threads[i] = (temp*num_mod)+i; }
   */
   Config::CoreList mod_list = g_config->getModuleList();   
   int* my_threads = new int[mod_list.size()];
   i = 0;
   for (Config::CLCI m = mod_list.begin(); m != mod_list.end(); ++m) {
      my_threads[i++] = *m;
   }

   // When using MPI, we can have multiple processes, each potentially
   //  containing multiple threads.  MPI treats each process (not each
   //  thread) as a communication endpoint with its own unique rank.
   //  Therefore, to communicate with a particular thread, we need to know
   //  the rank of the process that contains it.  Here we build a map from
   //  core (thread) number to MPI rank.  To do this, we ask every process
   //  for a list of the cores it contains.
  
   // First, find out how many cores each process contains
   MPI_Comm_size(MPI_COMM_WORLD, &temp);
   num_procs = temp;  // convert from int to UINT32
#ifdef PT_DEBUG
   cout << "Number of processes: " << num_procs << endl;
#endif
   thread_counts = new int[num_procs];
   MPI_Allgather(&num_mod, 1, MPI_INT, thread_counts, 1, MPI_INT, MPI_COMM_WORLD);
  
   // Create array of indexes that will be used to place values received
   //  from each process in the appropriate place in the receive buffer.
   displs = new int[num_procs];
   displs[0] = 0;
   for (i=1; i<num_procs; i++) {
     displs[i] = displs[i-1]+thread_counts[i-1];
   }

   // Create the receive buffer
   num_cores = displs[num_procs-1] + thread_counts[num_procs-1];
   ASSERTX(num_cores == g_config->totalMods());
#ifdef PT_DEBUG
   cout << "Total number of cores: " << num_cores << endl;
#endif
   rbuf = new int[num_cores];

   // Send out our core list and gather the lists from everyone else
   MPI_Allgatherv(&my_threads[0], num_mod, MPI_INT, rbuf, thread_counts,
		  displs, MPI_INT, MPI_COMM_WORLD);

   // Now we have a complete list of all the cores in each process.
   // Convert it into a map from core to process ID:
   dest_ranks = new int[num_cores];
#ifdef PT_DEBUG
   cout << "Core to MPI rank map: ";
#endif
   for (i=0; i<num_procs; i++) {
      for (j=displs[i]; j<(UINT32)(displs[i]+thread_counts[i]); j++) {
         dest_ranks[rbuf[j]] = i;
#ifdef PT_DEBUG
	 cout << rbuf[j] << "->" << i << ", ";
#endif
      }
   }
#ifdef PT_DEBUG
   cout << endl;
#endif
   
}

// This routine should be executed once in each thread
int Transport::ptInit(int tid, int num_mod)
{
   int MPI_rank;
   // tid is my thread ID
   pt_tid = tid;
   i_am_the_MCP = false;

   // comm_id is my communication network endpoint ID and is equivalent
   //  to module number

   // FIXME: Choose between a couple of fixed schemes until we get full
   //  support for multiple processes and multiple threads.  Eventually,
   //  we will either pick an ID from the list of IDs for this process,
   //  or our ID will be assigned by someone else and passed into this
   //  method.
   if (g_config->numProcs() == 1) {
      // If we only have one process, we can make comm_id equal to tid
      comm_id = tid;

   } else if (g_config->numProcs() == g_config->totalMods()) {
      // If the number of processes is equal to the number of modules, we
      //  have one module per process.  Therefore, we can just use the
      //  process's MPI rank as the comm_id.
      MPI_Comm_rank(MPI_COMM_WORLD, &MPI_rank);
      comm_id = MPI_rank;  // Convert from int to whatever comm_id is

   } else {
      // All other cases are currently unsupported
      cerr << "ERROR: Multiple processes each with multiple threads is not "
	   << "currently supported!" << endl;
      MPI_Abort(MPI_COMM_WORLD, -1);
      exit(-1);
   }

   return 0;
}

// This routine should be executed once in each thread
void Transport::ptInitMCP()
{
   int MPI_rank;
   i_am_the_MCP = true;
   pt_tid = -1;

   // Fill in the comm_id with the rank of my process (should always be
   //  zero for now)
   MPI_Comm_rank(MPI_COMM_WORLD, &MPI_rank);
   comm_id = MPI_rank;  // Convert from int to whatever comm_id is
   assert(comm_id == 0);

   return;
}

int Transport::ptSend(int receiver, char *buffer, int size)
{
   // Notes:
   //  - The data is sent using MPI_BYTE so that MPI won't do any conversions.
   //  - We use the receiver ID as the tag so that messages can be
   //    demultiplexed automatically by MPI in the receiving process.

#ifdef PT_DEBUG
   cout << "PT sending msg ==> tid:" << pt_tid
	<< ", comm_id:" << comm_id << ", recv:" << receiver
	<< ", size:" << size << " ...";
#endif
   MPI_Send(buffer, size, MPI_BYTE, dest_ranks[receiver], receiver,
	    MPI_COMM_WORLD);
#ifdef PT_DEBUG
   cout << "done." << endl;
#endif
   // FIXME: Why do we need to return the size?
   return size;
}

char* Transport::ptRecv()
{
   MPI_Status status;
   int pkt_size, source;
   char* buffer;

#ifdef PT_DEBUG
   cout << "PT (tid:" << pt_tid << ",cid:" << comm_id
	<< ") attempting receive..." << endl;
#endif

   // Probe for a message from any source but with our ID tag.
   // Use a blocking probe so that we wait until a message arrives.
   MPI_Probe(MPI_ANY_SOURCE, comm_id, MPI_COMM_WORLD, &status);
 
   // Now we know that there is a message ready, check status to see how
   //  big it is and who the source is.
   MPI_Get_count(&status, MPI_BYTE, &pkt_size);
   assert(status.MPI_SOURCE != MPI_UNDEFINED);
   source = status.MPI_SOURCE;
   
   // Allocate a buffer for the incoming message
   buffer = new char[pkt_size];

#ifdef PT_DEBUG
   cout << "PT found msg ==> tid:" << pt_tid
	<< ", comm_id:" << comm_id << ", source rank:" << source
	<< ", size:" << pkt_size << " ...";
#endif

   // We need to make sure the source here is the same as the one returned
   //  by the call to Probe above.  Otherwise, we might get a message from
   //  a different sender that could have a different size.
   MPI_Recv(buffer, pkt_size, MPI_BYTE, source, comm_id, MPI_COMM_WORLD,
	    &status);
   
#ifdef PT_DEBUG
   cout << "received." << endl;
#endif

   return buffer;
   // NOTE: the caller should free the buffer when it's finished with it
}

bool Transport::ptQuery()
{
   int flag;
   MPI_Status status;

   // Probe for a message from any source but with our ID tag
   MPI_Iprobe(MPI_ANY_SOURCE, comm_id, MPI_COMM_WORLD, &flag, &status);

   // flag == 0 indicates that no message is waiting
   return (flag != 0);
}

void Transport::ptSendToMCP(UInt8* buffer, UInt32 num_bytes)
{
   // Arguments:
   //   buffer = input, pointer to buffer of data to send
   //   num_bytes = input, number of bytes to send starting from buffer
   // Notes:
   //  - The data is sent using MPI_BYTE so that MPI won't do any conversions.
   //  - The MCP should always be located in the process with MPI rank 0
   //  - We use MCP_tag to indicate messages to the MCP

#ifdef PT_DEBUG
   cout << "PT sending msg to MCP ==> tid:" << pt_tid
	<< ", comm_id:" << comm_id <<
	<< ", size:" << num_bytes << " ... ";
#endif
   //MPI_Send(data ptr, # elms, elm type, rank, tag, communicator);
   MPI_Send((char*)buffer, num_bytes, MPI_BYTE, 0, MCP_tag, MPI_COMM_WORLD);
#ifdef PT_DEBUG
   cout << "done." << endl;
#endif

   return;
}

UInt8* Transport::ptRecvFromMCP(UInt32* num_bytes)
{
   MPI_Status status;

#ifdef PT_DEBUG
   cout << "PT (tid:" << pt_tid << ",cid:" << comm_id
	<< ") attempting receive from MCP ..." << endl;
#endif

   // Probe for a message from the MCP process with our MCP comm tag.
   // Use a blocking probe so that we wait until a message arrives.
   int MCP_comm_tag = (MCP_tag-1) - comm_id;
   MPI_Probe(0, MCP_comm_tag, MPI_COMM_WORLD, &status);
 
   // Now we know that there is a message ready, check status to see how
   //  big it is.
   int pkt_size;
   MPI_Get_count(&status, MPI_BYTE, &pkt_size);
   *num_bytes = pkt_size;
   
   // Allocate a buffer for the incoming message
   UInt8* buffer = new UInt8[*num_bytes];

#ifdef PT_DEBUG
   cout << "PT found msg from MCP ==> tid:" << pt_tid
	<< ", comm_id:" << comm_id
	<< ", size:" << *num_bytes << " ...";
#endif

   MPI_Recv(buffer, *num_bytes, MPI_BYTE, 0, MCP_comm_tag, MPI_COMM_WORLD,
	    &status);
   
#ifdef PT_DEBUG
   cout << "received." << endl;
#endif

   return buffer;
   // NOTE: the caller should free the buffer when it's finished with it
}

void Transport::ptMCPSend(UInt32 dest, UInt8* buffer, UInt32 num_bytes)
{
   // Notes:
   //  - The data is sent using MPI_BYTE so that MPI won't do any conversions.
   //  - We can't use the destination comm_id as the tag because then
   //    messages could get mixed up with regular user messages.  Instead
   //    we pick a tag by subtracting our comm_id from the MCP_tag (which
   //    should be the maximum allowable tag)

#ifdef PT_DEBUG
   cout << "PT (MCP) sending msg ==> dest:" << dest
	<< ", size:" << num_bytes << " ... ";
#endif
   MPI_Send(buffer, num_bytes, MPI_BYTE, dest_ranks[dest], (MCP_tag-1)-dest,
	    MPI_COMM_WORLD);
#ifdef PT_DEBUG
   cout << "done." << endl;
#endif

   return;
}

UInt8* Transport::ptMCPRecv(UInt32* num_bytes)
{
   MPI_Status status;
   int pkt_size, source;
   UInt8* buffer;

#ifdef PT_DEBUG
   cout << "PT (MCP) attempting receive..." << endl;
#endif

   // Probe for a message from any source but with the MCP tag.
   // Use a blocking probe so that we wait until a message arrives.
   MPI_Probe(MPI_ANY_SOURCE, MCP_tag, MPI_COMM_WORLD, &status);
 
   // Now we know that there is a message ready, check status to see how
   //  big it is and who the source is.
   MPI_Get_count(&status, MPI_BYTE, &pkt_size);
   assert(status.MPI_SOURCE != MPI_UNDEFINED);
   source = status.MPI_SOURCE;
   *num_bytes = pkt_size;
   
   // Allocate a buffer for the incoming message
   buffer = new UInt8[*num_bytes];

#ifdef PT_DEBUG
   cout << "PT (MCP) found msg ==> source rank:" << source
	<< ", size:" << *num_bytes << " ...";
#endif

   // We need to make sure the source here is the same as the one returned
   //  by the call to Probe above.  Otherwise, we might get a message from
   //  a different sender that could have a different size.
   MPI_Recv(buffer, *num_bytes, MPI_BYTE, source, MCP_tag, MPI_COMM_WORLD,
	    &status);
   
#ifdef PT_DEBUG
   cout << "received." << endl;
#endif

   return buffer;
   // NOTE: the caller should free the buffer when it's finished with it
}