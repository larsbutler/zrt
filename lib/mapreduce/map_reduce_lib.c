/*
 * map_reduce_lib.c
 *
 *      Author: yaroslav
 */

#include <stdlib.h> //calloc
#include <stdio.h>  //puts
#include <string.h> //memcpy
#include <unistd.h> //ssize_t
#include <sys/types.h> //temp read file
#include <sys/stat.h> //temp read file
#include <fcntl.h> //temp read file
#include <assert.h> //assert
#include <alloca.h>

#include "map_reduce_lib.h"
#include "mr_defines.h"
#include "elastic_mr_item.h"
#include "eachtoother_comm.h"
#include "channels_conf.h"
#include "buffered_io.h"

#include "buffer.h"

/*this option disables macros WRITE_LOG_BUFFER */
#define DISABLE_WRITE_LOG_BUFFER

#define MAX_UINT32 4294967295
#define HASH_SIZE __mrif->data.hash_size
#define ALLOC_HASH_IN_STACK alloca(HASH_SIZE)

typedef int exclude_flag_t;
#define MAP_NODE_NO_EXCLUDE 0
#define MAP_NODE_EXCLUDE 1

struct MapReduceUserIf *__mrif = NULL;
#define PRINTABLE_HASH( hash) \
    __mrif->HashAsString( (char*)alloca((HASH_SIZE)*2+1), (hash), __mrif->data.hash_size)

#define HASH_COPY(dest, src) memcpy((dest), (src), __mrif->data.hash_size)
#define HASH_CMP(h1_p, h2_p) __mrif->HashComparator( (h1_p), (h2_p) )

/*Buffer item size used by mapreduce library*/
#define MRITEM_SIZE (__mrif->data.mr_item_size)

#define BOUNDS_OK(item_index, items_count)  (item_index) < (items_count)

#if defined(DEBUG) && !defined(DISABLE_WRITE_LOG_BUFFER)
#  define WRITE_LOG_BUFFER( map )					\
    if ( (map).header.count ){						\
	ElasticBufItemData *data;					\
	for (int i_=0; i_ < (map).header.count; i_++){			\
	    data = (ElasticBufItemData *)BufferItemPointer(&(map), i_);	\
	    WRITE_FMT_LOG( "[%d] %s\n",					\
			   i_, PRINTABLE_HASH(&data->key_hash) );	\
	}								\
	fflush(0);							\
    }
#else
#  define WRITE_LOG_BUFFER(map)
#endif

static int 
ElasticBufItemHashQSortComparator(const void *p1, const void *p2){
    return __mrif->HashComparator( &((ElasticBufItemData*)p1)->key_hash,
				   &((ElasticBufItemData*)p2)->key_hash );
}

static size_t 
MapInputDataProvider( int fd, 
		      char **input_buffer, 
		      size_t requested_buf_size, 
		      int unhandled_data_pos ){
    WRITE_FMT_LOG("MapInputDataProvider *input_buffer=%p, fd=%d, "
		  "requested_buf_size=%u, unhandled_data_pos=%d\n",
		  *input_buffer, fd, (uint32_t)requested_buf_size, unhandled_data_pos );
    size_t rest_data_in_buffer = requested_buf_size - unhandled_data_pos;
    int first_call = *input_buffer == NULL ? 1 : 0;
    if ( !*input_buffer ){
	/*if input buffer not yet initialized then alloc it. 
	  It should be deleted by our framework after use*/
	*input_buffer = calloc( requested_buf_size+1, sizeof(char) );
	/*no data in buffer, it seems to be a first call of DataProvider*/
	rest_data_in_buffer = 0;
    }

    /**********************************************************************
     * Check for unsupported data handling. User should get handled data pos,
     * and we should concatenate unprocessed data with new data from input file.
     * For first launch of DataProvider unhandled_data_pos is ignoring*/
    if ( !first_call ){
	if ( !unhandled_data_pos ){
	    /* user Map function not hadnled data at all. In any case user Map must
	     * handle data, but in case if insufficiently of data in input buffer
	     * then user should increase buffer size via MAP_CHUNK_SIZE*/
	    assert( unhandled_data_pos );
	}
	else if ( unhandled_data_pos < requested_buf_size/2 ){
	    /* user Map function hadnled less than half of data. In normal case we need
	     * to move the rest of data to begining of buffer, but we can't do it just
	     * move because end of moved data and start of unhandled data is overlaps;
	     * For a while we don't want create another heap buffers to join rest data 
	     * and new data; */
	    assert( unhandled_data_pos > requested_buf_size/2 );
	}
	else if ( unhandled_data_pos != requested_buf_size )
	    {
		/*if not all data processed by previous call of DataProvider 
		 *then copy the rest into input_buffer*/
		memcpy( (*input_buffer), 
			(*input_buffer)+unhandled_data_pos, 
			rest_data_in_buffer );
	    }
    }
    ssize_t readed = read( fd, 
			   *input_buffer, 
			   requested_buf_size - rest_data_in_buffer );

    WRITE_FMT_LOG("MapInputDataProvider OK return handled bytes=%u\n", 
		  (uint32_t)(rest_data_in_buffer+readed) );
    return rest_data_in_buffer+readed;
}


static void 
LocalSort( Buffer *sortable ){
    /*sort created array*/
    qsort( sortable->data, 
	   sortable->header.count, 
	   sortable->header.item_size, 
	   ElasticBufItemHashQSortComparator );
}


size_t
GetHistogram( const Buffer* map, 
	      int step, 
	      Histogram *histogram ){
    /*access hash keys in map buffer and add every N hash into histogram*/
    const ElasticBufItemData* mritem;

    if ( !map->header.count ) return 0;
    if ( !step ){
	mritem = (const ElasticBufItemData*) BufferItemPointer(map, map->header.count-1);
	AddBufferItem( &histogram->buffer, &mritem->key_hash );
	histogram->step_hist_common = histogram->step_hist_last = map->header.count;
    }
    else{
	histogram->step_hist_common = step;
	int i = -1; /*array index*/
	do{
	    histogram->step_hist_last = MIN( step, map->header.count-i-1 );
	    i += histogram->step_hist_last;
	    mritem = (const ElasticBufItemData*) BufferItemPointer(map, i);
	    AddBufferItem( &histogram->buffer, &mritem->key_hash );
	}while( i < map->header.count-1 );

    }
#ifdef DEBUG
    WRITE_LOG("\nHistogram=[");
    uint8_t* hash;
    int i;
    for( i=0; i < histogram->buffer.header.count ; i++ ){
	hash = (uint8_t*) BufferItemPointer( &histogram->buffer, i);
	WRITE_FMT_LOG("%s ", PRINTABLE_HASH(hash) );
    }
    WRITE_LOG("]\n"); fflush(0);
#endif
    return histogram->buffer.header.count;
}

/************************************************************************
 * EachToOtherPattern callback.
 * Every map node read histograms from another nodes*/
void 
ReadHistogramFromNode( struct EachToOtherPattern *p_this, 
		       int nodetype, 
		       int index, 
		       int fdr ){
    WRITE_FMT_LOG( "eachtoother:read( from:index=%d, from:fdr=%d)\n", index, fdr );
    /*read histogram data from another nodes*/
    struct MapReduceData *mapred_data = (struct MapReduceData *)p_this->data;
    /*histogram has always the same index as node in nodes_list reading from*/
    assert( index < mapred_data->histograms_count );
    Histogram *histogram = &mapred_data->histograms_list[index];
    WRITE_FMT_LOG("ReadHistogramFromNode index=%d\n", index);
    /*histogram array should be empty before reading*/
    assert( !histogram->buffer.header.count );
    read(fdr, histogram, sizeof(Histogram)-sizeof(HashBuffer) );
    size_t items_count;
    read(fdr, &items_count, sizeof(size_t) );
    if ( items_count ){
	AllocBuffer( &histogram->buffer, HASH_SIZE, items_count );
	read(fdr, histogram->buffer.data, histogram->buffer.header.buf_size );
    }
}

/************************************************************************
 * EachToOtherPattern callbacks.
 * Every map node write histograms to another nodes*/
void 
WriteHistogramToNode( struct EachToOtherPattern *p_this, 
		      int nodetype, 
		      int index, 
		      int fdw ){
    WRITE_FMT_LOG( "eachtoother:write( to:index=%d, to:fdw=%d)\n", index, fdw );
    /*write own data histogram to another nodes*/
    struct MapReduceData *mapred_data = (struct MapReduceData *)p_this->data;
    /*nodeid in range from 1 up to count*/
    int own_histogram_index = p_this->conf->ownnodeid-1;
    assert( own_histogram_index < mapred_data->histograms_count );
    Histogram *histogram = &mapred_data->histograms_list[own_histogram_index];
    WRITE_FMT_LOG("WriteHistogramToNode index=%d\n", index);
    /*write Histogram struct, array contains pointer it should be ignored by receiver */
    write(fdw, histogram, sizeof(Histogram)-sizeof(HashBuffer));
    /*write histogram data size*/
    write(fdw, &histogram->buffer.header.buf_size, sizeof(size_t) );
    /*write histogram data size*/
    if ( histogram->buffer.header.buf_size ){
	write(fdw, 
	      histogram->buffer.data, 
	      histogram->buffer.header.buf_size );
    }
}


/*******************************************************************************
 * Summarize Histograms, shrink histogram and get as dividers array*/



void 
GetReducersDividerArrayBasedOnSummarizedHistograms( Histogram *histograms, 
						    int histograms_count, 
						    Buffer *divider_array ){
int CalculateItemsCountBasedOnHistograms(Histogram *histograms, int histograms_count );
int SearchMinimumHashAmongCurrentItemsOfAllHistograms( Histogram *histograms, 
						       int *current_indexes_array,
						       int histograms_count );

    WRITE_LOG("Create dividers list");
    assert(divider_array);
    int dividers_count_max = divider_array->header.buf_size / divider_array->header.item_size;
    assert(dividers_count_max != 0);
    size_t size_all_histograms_data = 0;
    size_all_histograms_data = 
	CalculateItemsCountBasedOnHistograms(histograms, histograms_count );
    /*calculate block size divider for single reducer*/
    size_t generic_divider_block_size = size_all_histograms_data / dividers_count_max;
    size_t current_divider_block_size = 0;

    /*start histograms processing*/
    int indexes[dividers_count_max];
    memset( &indexes, '\0', sizeof(indexes) );
    int histogram_index_with_minimal_hash = 0;
    
    /*calculate dividers in loop, last divider item must be added after loop 
      with maximum value as possible*/
    while( divider_array->header.count+1 < dividers_count_max ){
	histogram_index_with_minimal_hash = 
	    SearchMinimumHashAmongCurrentItemsOfAllHistograms( histograms, 
							       indexes, 
							       histograms_count );
	if ( histogram_index_with_minimal_hash == -1 ){
	    /*error - no more items in histograms, leave loop*/
	    break;
	}
	/*increase current divider block size*/
	if ( BOUNDS_OK(indexes[histogram_index_with_minimal_hash], 
		       histograms[histogram_index_with_minimal_hash].buffer.header.count-1)){
	    current_divider_block_size += histograms[histogram_index_with_minimal_hash]
		.step_hist_common;
	}
	else{
	    current_divider_block_size += histograms[histogram_index_with_minimal_hash]
		.step_hist_last;
	}

	if ( current_divider_block_size >= generic_divider_block_size ){
	    int min_item_index = indexes[histogram_index_with_minimal_hash];
	    Histogram* min_item_hist = &histograms[histogram_index_with_minimal_hash];
	    const uint8_t* divider_hash	
		= (const uint8_t*)BufferItemPointer(&min_item_hist->buffer, 
						    min_item_index);
	    /*add located divider value to dividers array*/
	    AddBufferItem( divider_array, divider_hash );
	    current_divider_block_size=0;

	    WRITE_FMT_LOG( "histogram#=%d, item#=%d, hash=%s\n", 
			   histogram_index_with_minimal_hash, min_item_index,
			   PRINTABLE_HASH(divider_hash) );
	    fflush(0);
	}
	indexes[histogram_index_with_minimal_hash]++;
	/*while pre last divider processing*/
    }

    /*Add last divider hash with maximum value, and it is guaranties that all rest 
      map values with hash value less than maximum will distributed into last reduce node 
      appropriated to last divider*/
    uint8_t* divider_hash = alloca(HASH_SIZE);
    memset( divider_hash, 0xffffff, HASH_SIZE );
    AddBufferItem(divider_array, divider_hash);

#ifdef DEBUG
    WRITE_LOG("\ndivider_array=[");
    for( int i=0; i < dividers_count_max; i++ ){
	const uint8_t* current_hash = (const uint8_t*)BufferItemPointer(divider_array, i);
	WRITE_FMT_LOG("%s ", PRINTABLE_HASH(current_hash) );
    }
    WRITE_LOG("]\n");
#endif
}

int
CalculateItemsCountBasedOnHistograms(Histogram *histograms, int hist_count ){
    uint32_t ret=0;
    for ( int i=0; i < hist_count; i++ ){
	WRITE_FMT_LOG("histogram #%d items count=%u\n", i,
		      histograms[i].buffer.header.count );
	/*if histogram has more than one item*/
	if ( histograms[i].buffer.header.count > 1 ){
	    ret+= histograms[i].step_hist_common * (histograms[i].buffer.header.count -1); 
	    ret+= histograms[i].step_hist_last;			
	}								
	else if ( histograms[i].buffer.header.count == 1 ){		
	    assert( histograms[i].step_hist_last == histograms[i].step_hist_common ); 
	    ret += histograms[i].step_hist_last;			
	}							       
    }
    WRITE_FMT_LOG("all histograms items count=%u\n", ret);
    return ret;
}


int
SearchMinimumHashAmongCurrentItemsOfAllHistograms( Histogram *histograms, 
						   int *current_indexes_array,
						   int histograms_count){ 
    int res = -1;
    const uint8_t* current_hash;
    const uint8_t* minimal_hash = NULL;
    /*found minimal value among currently indexed histogram values*/
    for ( int i=0; i < histograms_count; i++ ){ /*loop for histograms*/
	/*check bounds of current histogram*/
	if ( BOUNDS_OK(current_indexes_array[i],
		       histograms[i].buffer.header.count) ){
	    /*get minimal value among currently indexed histogram values*/ 
	    current_hash = (const uint8_t*)BufferItemPointer( &histograms[i].buffer,
							      current_indexes_array[i]);
	    if ( !minimal_hash || HASH_CMP( current_hash, minimal_hash ) <= 0 ){
		res = i;
		minimal_hash = current_hash;
	    }
	}
    }
    return res;
}


size_t
MapInputDataLocalProcessing( const char *buf, 
			     size_t buf_size, 
			     int last_chunk, 
			     Buffer *result ){
    size_t unhandled_data_pos = 0;
    Buffer sort;
    if( AllocBuffer( &sort, MRITEM_SIZE, 1024 /*granularity*/ ) != 0 ){
	assert(0);
    }

    WRITE_FMT_LOG("sbrk()=%d\n", sbrk(0) );
    WRITE_FMT_LOG("======= new portion of data read: input buffer=%p, buf_size=%u\n", 
		  buf, (uint32_t)buf_size );
    /*user Map process input data and allocate keys, values buffers*/
    unhandled_data_pos = __mrif->Map( buf, buf_size, last_chunk, &sort );
    WRITE_FMT_LOG("User Map() function result : items count=%u, unhandled pos=%u\n",
		  (uint32_t)sort.header.count, (uint32_t)unhandled_data_pos );

    WRITE_LOG_BUFFER( sort );
    LocalSort( &sort);

    WRITE_FMT_LOG("MapCallEvent:sorted map, count=%u\n", (uint32_t)sort.header.count);
    WRITE_LOG_BUFFER( sort );

    if ( AllocBuffer(result, MRITEM_SIZE, sort.header.count/4 ) !=0 ){
	assert(0);
    }

    WRITE_FMT_LOG("MapCallEvent: Combine Start, count=%u\n", (uint32_t)sort.header.count);
    if ( __mrif->Combine ){
	__mrif->Combine( &sort, result );
	FreeBufferData(&sort);
    }
    else{
	//use sort buffer instead reduced, because user does not defined Combine function
	*result = sort;
	sort.data = NULL;
    }

    WRITE_FMT_LOG("MapCallEvent: Combine Complete, count=%u\n", (uint32_t)result->header.count);
    WRITE_LOG_BUFFER( *result );
    return unhandled_data_pos;
}


void 
MapCreateHistogramSendEachToOtherCreateDividersList( struct ChannelsConfigInterface *ch_if, 
						     struct MapReduceData *data, 
						     const Buffer *map ){
    /*retrieve Map/Reducer nodes list and nodes count*/
    int *map_nodes_list = NULL;
    int map_nodes_count = ch_if->GetNodesListByType( ch_if, EMapNode, &map_nodes_list );
    assert(map_nodes_count); /*it should not be zero*/

    /*get reducers count, this same as dividers count for us, reducers list is not needed now*/
    int *reduce_nodes_list = NULL;
    int reduce_nodes_count = 
	ch_if->GetNodesListByType( ch_if, EReduceNode, &reduce_nodes_list );

    //generate histogram with offset
    int current_node_index = ch_if->ownnodeid-1;

    struct Histogram* histogram = &data->histograms_list[current_node_index];

    size_t hist_step = map->header.count / 100 / map_nodes_count;
    /*save histogram for own data always into current_node_index-pos of histograms array*/
    GetHistogram( map, hist_step, histogram );

    WRITE_FMT_LOG("MapCallEvent: histogram created. "
		  "count=%d, fstep=%u, lstep=%u\n",
		  (int)histogram->buffer.header.count,
		  (uint16_t)histogram->step_hist_common,
		  (uint16_t)histogram->step_hist_last );

    /*use eachtoother pattern to map nodes communication*/
    struct EachToOtherPattern histogram_from_all_to_all_pattern = {
	.conf = ch_if,
	.data = data,
	.Read = ReadHistogramFromNode,
	.Write = WriteHistogramToNode
    };
    StartEachToOtherCommunication( &histogram_from_all_to_all_pattern, EMapNode );

    /*Preallocate buffer space and exactly for items count=reduce_nodes_count */
    AllocBuffer( &data->dividers_list, HASH_SIZE, reduce_nodes_count);
    /*From now every map node contain histograms from all map nodes, summarize histograms,
     *to get distribution of all data. Result of summarization write into divider_array.*/
    GetReducersDividerArrayBasedOnSummarizedHistograms(
						       data->histograms_list,
						       data->histograms_count,
						       &data->dividers_list );
    free(map_nodes_list);
    free(reduce_nodes_list);
}

static int
BufferedWriteSingleMrItem( BufferedIOWrite* bio, 
			   int fdw,
			   const ElasticBufItemData* item ){
    int bytes=0;
    /*key size*/
    bytes+=bio->write( bio, fdw, (void*)&item->key_data.size, sizeof(item->key_data.size));
    /*key data*/
    bytes+=bio->write( bio, fdw, (void*)item->key_data.addr, item->key_data.size);

    if ( __mrif->data.value_is_data ){
	/*ElasticBufItemData::key_data::addr used as data, key_data::size not used*/
	bytes+=bio->write( bio, fdw, (void*)&item->value.addr, sizeof(item->value.addr) );
    }
    else{
	/*value size*/
	bytes+=bio->write( bio, fdw, (void*)&item->value.size, sizeof(item->value.size));
	/*value data*/
	bytes+=bio->write( bio, fdw,(void*)item->value.addr, item->value.size);
    }
    /*hash of key*/
    bytes+=bio->write( bio, fdw, (void*)&item->key_hash, HASH_SIZE);
    return bytes;
}

static int
BufferedReadSingleMrItem( BufferedIORead* bio, 
			  int fdr,
			  ElasticBufItemData* item ){
    int bytes=0;
    /*key size*/
    bytes += bio->read( bio, fdr, (void*)&item->key_data.size, sizeof(item->key_data.size));
    /*key data*/
    item->key_data.addr = (uintptr_t)malloc(item->key_data.size);
    item->own_key = EDataOwned;
    bytes += bio->read( bio, fdr, (void*)item->key_data.addr, item->key_data.size);

    if ( __mrif->data.value_is_data ){
	/*ElasticBufItemData::key_data::addr used as data, key_data::size not used*/
	bytes += bio->read( bio, fdr, (void*)&item->value.addr, sizeof(item->value.addr) );
	item->own_value = EDataNotOwned;
	item->value.size = 0;
    }
    else{

	/*value size*/
	bytes += bio->read( bio, fdr, (void*)&item->value.size, sizeof(item->value.size));
	/*value data*/
	item->value.addr = (uintptr_t)malloc(item->value.size);
	item->own_value=EDataOwned;
	bytes += bio->read( bio, fdr, (void*)item->value.addr, item->value.size);
    }
    /*key hash*/
    bytes += bio->read( bio, fdr, (void*)&item->key_hash, HASH_SIZE);
    return bytes;
}
 
 
void 
WriteDataToReduce( BufferedIOWrite* bio, 
		    int fdw, 
		    const Buffer *map, 
		    int data_start_index, 
		    int items_count,
		    int last_data_flag ){
    ElasticBufItemData* current = alloca( MRITEM_SIZE );

    /*log first and last hashes of range to send */
#ifdef DEBUG
    WRITE_FMT_LOG( "data_start_index=%d, items_count=%d\n", 
		   data_start_index, items_count );
    ElasticBufItemData* temp = alloca( MRITEM_SIZE );
    GetBufferItem( map, data_start_index, current );
    GetBufferItem( map, data_start_index+items_count-1, temp ); /*last item from range*/
    WRITE_FMT_LOG( "send range of hashes [%s - %s]",
		   PRINTABLE_HASH(&current->key_hash), 
		   PRINTABLE_HASH(&temp->key_hash) );
    WRITE_FMT_LOG( "fdw=%d, last_data_flag=%d, items_count=%d\n", 
		   fdw, last_data_flag, items_count );
#endif //DEBUG
    /*write last data flag 0 | 1, if reducer receives 1 then it
      should exclude this map node from communications*/
    bio->write( bio, fdw, &last_data_flag, sizeof(int) );
    /*items count we want send to a single reducer*/
    bio->write( bio, fdw, &items_count, sizeof(int) );

    int loop_up_to_count = data_start_index+items_count;
    assert( loop_up_to_count <= map->header.count );
    for( int i=data_start_index; i < loop_up_to_count; i++ ){
	GetBufferItem( map, i, current );
	BufferedWriteSingleMrItem( bio, fdw, current );
    }
    bio->flush_write(bio, fdw);
}


static void 
MapSendToAllReducers( struct ChannelsConfigInterface *ch_if, 
		      int last_data, 
		      const Buffer *map){
    /*get reducers count, this is same as dividers count for us*/
    int *reduce_nodes_list = NULL;
    int dividers_count = ch_if->GetNodesListByType( ch_if, EReduceNode, &reduce_nodes_list );
    assert(reduce_nodes_list);

    /*send keys-values to Reducer nodes, using dividers data from data_start_index to
     * data_end_index range their max value less or equal to current divider value */
    int data_start_index = 0;
    int count_in_section = 0;
    ElasticBufItemData* current = alloca( MRITEM_SIZE );
    void*  current_divider_hash = ALLOC_HASH_IN_STACK;

    /*Get first divider hash*/
    GetBufferItem(&__mrif->data.dividers_list, 0, current_divider_hash);

    int current_divider_index = 0;

    /*Declare pretty big buffer to provide efficient buffered IO, 
      free it at the function end*/
    void *send_buffer = malloc(TEMP_BUFFER_SIZE);
    BufferedIOWrite* bio = AllocBufferedIOWrite( send_buffer, TEMP_BUFFER_SIZE);

    /*loop for data*/
    for ( int j=0; j < map->header.count; j++ ){
	GetBufferItem( map, j, current);
	if ( HASH_CMP(&current->key_hash, current_divider_hash ) <= 0 ){
	    count_in_section++;
	}

	/*if current item is last OR current hash more than current divider key*/
	if ( j+1 == map->header.count ||
	     HASH_CMP(&current->key_hash, current_divider_hash ) > 0 ){
#ifdef DEBUG
	    if ( j > 0 ){
		ElasticBufItemData* previtem = alloca( MRITEM_SIZE );
		GetBufferItem( map, j-1, previtem);
		WRITE_FMT_LOG( "reducer #%d divider_hash=%s, [%d]=%s\n",
			       reduce_nodes_list[current_divider_index],
			       PRINTABLE_HASH(current_divider_hash),
			       j-1, PRINTABLE_HASH(&previtem->key_hash) );
	    }
	    WRITE_FMT_LOG( "reducer #%d divider_hash=%s, [%d]=%s\n",
			   reduce_nodes_list[current_divider_index],
			   PRINTABLE_HASH(current_divider_hash),
			   j, PRINTABLE_HASH(&current->key_hash) );
#endif
	    /*send to reducer with current_divider_index index*/
	    struct UserChannel *channel 
		= ch_if->Channel(ch_if,
				 EReduceNode, 
				 reduce_nodes_list[current_divider_index], 
				 EChannelModeWrite);
	    assert(channel);
	    int fdw = channel->fd;
	    last_data = last_data ? MAP_NODE_EXCLUDE : MAP_NODE_NO_EXCLUDE;
	    WRITE_FMT_LOG( "fdw=%d to reducer node %d write %d items\n", fdw,
			   reduce_nodes_list[current_divider_index], (int)count_in_section );
	    WriteDataToReduce( bio,
			       fdw, 
			       map, 
			       data_start_index, 
			       count_in_section, 
			       last_data );

	    /*set start data index of next divider*/
	    data_start_index = j+1;
	    /*reset count for next divider*/
	    count_in_section = 0;
	    /*switch to next divider, do increment if it's not last handling item*/
	    if ( j+1 < map->header.count ){
		current_divider_index++;
		/*retrieve hash value by updated index*/
		GetBufferItem( &__mrif->data.dividers_list, 
			       current_divider_index, 
			       current_divider_hash);
	    }
	    assert( current_divider_index < dividers_count );
	}
    }

    /*if map is empty then nothing sent to reduce nodes. But every map node should
     *send at least one time to every reduce node last_data flag and little bit*/
    if ( !map->header.count ){
	for ( int i=0; i < dividers_count; i++ ){
            struct UserChannel *channel 
		= ch_if->Channel(ch_if, 
				 EReduceNode, 
				 reduce_nodes_list[i], 
				 EChannelModeWrite);
            assert(channel);
	    int fdw = channel->fd;
	    WRITE_FMT_LOG( "fdw=%d write to reducer MAP_NODE_EXCLUDE\n", fdw );
	    WriteDataToReduce( bio, fdw, map, 0, 0, MAP_NODE_EXCLUDE );
	}
    }

    free(bio);
    free(send_buffer);
    free(reduce_nodes_list);
}

void 
InitMapInternals( struct MapReduceUserIf *mrif, 
		  const struct ChannelsConfigInterface *chif, 
		  struct MapNodeEvents* ev){
    /*get histograms count for MapReduceData*/
    int *nodes_list_unwanted = NULL;
    mrif->data.histograms_count = 
	chif->GetNodesListByType(chif, EMapNode, &nodes_list_unwanted );
    assert(mrif->data.histograms_count>0);
    free(nodes_list_unwanted);
    /*init histograms list*/
    mrif->data.histograms_list = calloc( mrif->data.histograms_count, 
					 sizeof(*mrif->data.histograms_list) );
    for(int i=0; i<mrif->data.histograms_count; i++){
	AllocBuffer(&mrif->data.histograms_list[i].buffer, mrif->data.hash_size, 100);
    }

    ev->MapCreateHistogramSendEachToOtherCreateDividersList 
	= MapCreateHistogramSendEachToOtherCreateDividersList;
    ev->MapInputDataLocalProcessing = MapInputDataLocalProcessing;
    ev->MapInputDataProvider = MapInputDataProvider;
    ev->MapSendToAllReducers = MapSendToAllReducers;
}


int 
MapNodeMain( struct MapReduceUserIf *mrif, 
	     struct ChannelsConfigInterface *chif ){
    WRITE_LOG("MapNodeMain\n");
    assert(mrif);
    assert(mrif->Map);
    assert(mrif->Combine);
    assert(mrif->Reduce);
    assert(mrif->HashComparator);
#ifndef DISABLE_WRITE_LOG_BUFFER
    assert(mrif->HashAsString);
#endif

    __mrif = mrif;

    struct MapNodeEvents events;
    InitMapInternals(mrif, chif, &events);

    /*should be initialized at fist call of DataProvider*/
    char *buffer = NULL;
    size_t returned_buf_size = 0;
    /*default block size for input file*/
    size_t split_input_size=DEFAULT_MAP_CHUNK_SIZE_BYTES;

    /*get from environment block size for input file*/
    if ( getenv(MAP_CHUNK_SIZE_ENV) )
	split_input_size = atoi(getenv(MAP_CHUNK_SIZE_ENV));
    WRITE_FMT_LOG( "MAP_CHUNK_SIZE_BYTES=%d\n", split_input_size );
	
    /*by default can set any number, but actually it should be point to start of unhandled data,
     * for fully handled data it should be set to data size*/
    size_t current_unhandled_data_pos = 0;
    int last_chunk = 0;

    /*get input channel*/
    struct UserChannel *channel = chif->Channel(chif,
						EInputOutputNode, 
						1, 
						EChannelModeRead);
    assert(channel);

    /*read input data*/
    do{
	free(buffer), buffer=NULL;

	/*last parameter is not used for first call, 
	  for another calls it should be assigned by user returned value of Map call*/
	returned_buf_size = events.MapInputDataProvider(
							channel->fd,
							&buffer,
							split_input_size,
							current_unhandled_data_pos
							);
	last_chunk = returned_buf_size < split_input_size? 1 : 0; //last chunk flag
	if ( last_chunk != 0 ){
	    WRITE_LOG( "MapInputDataProvider last chunk data" );
	}

	/*prepare Buffer before using by user Map function*/
	Buffer map_buffer;

	if ( returned_buf_size ){
	    /*call users Map, Combine functions only for non empty data set*/
	    current_unhandled_data_pos = 
		events.MapInputDataLocalProcessing( buffer, 
						    returned_buf_size,
						    last_chunk,
						    &map_buffer );
	}

	if ( !mrif->data.dividers_list.header.count ){
	    events.MapCreateHistogramSendEachToOtherCreateDividersList( chif, 
									&mrif->data, 
									&map_buffer );
	}

	/*based on dividers list which helps easy distribute data to reduce nodes*/
	events.MapSendToAllReducers( chif, 
				     last_chunk, 
				     &map_buffer);

	for(int i=0; i < map_buffer.header.count; i++){
	    TRY_FREE_MRITEM_DATA( (ElasticBufItemData*) 
				  BufferItemPointer(&map_buffer, i ) );
	}
	FreeBufferData(&map_buffer);
    }while( last_chunk == 0 );

    WRITE_LOG("MapNodeMain Complete\n");

    return 0;
}

/*Copy into dest buffer items from source_arrays buffers in sorted order*/
static void 
MergeBuffersToNew( Buffer *dest,
		   const Buffer *source_arrays, 
		   int arrays_count ){
    int SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( const Buffer* merge_buffers, 
							     int *current_indexes_array,
							     int count);
#ifdef DEBUG
    /*List of items count of every merging item before merge*/
    int i;
    for(i=0; i < arrays_count; i++ ){
	WRITE_FMT_LOG( "Before merge: source[#%d], keys count %d\n", 
		       i, source_arrays[i].header.count );
    }    
#endif //DEBUG
    int merge_pos[arrays_count];
    memset(merge_pos, '\0', sizeof(merge_pos) );
    int min_key_array = -1;
    ElasticBufItemData* current;

    /*search minimal key among current items of source_arrays*/
    min_key_array =
	SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( source_arrays, 
							     merge_pos,
							     arrays_count);
    while( min_key_array !=-1 ){
	/*copy item data with minimal key into destination buffer*/
	current = (ElasticBufItemData*)
	    BufferItemPointer( &source_arrays[min_key_array], merge_pos[min_key_array] );
	AddBufferItem( dest, current );
	merge_pos[min_key_array]++;
	min_key_array =
	    SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( source_arrays, 
								 merge_pos,
								 arrays_count);
    }
}

int
SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( const Buffer* merge_buffers, 
						     int *current_indexes_array,
						     int count){ 
    int res = -1;
    const uint8_t* current_hash;
    const uint8_t* minimal_hash = NULL;
    for ( int i=0; i < count; i++ ){ /*loop for merge arrays*/
	/*check bounds of current buffer*/
	if ( BOUNDS_OK(current_indexes_array[i],merge_buffers[i].header.count) ){
	    /*get minimal value among currently indexed histogram values*/ 
	    current_hash = (const uint8_t*)BufferItemPointer( &merge_buffers[i],
							      current_indexes_array[i]);
	    if ( !minimal_hash || HASH_CMP( current_hash, minimal_hash ) <= 0 ){
		res = i;
		minimal_hash = current_hash;
	    }
	}
    }
    return res;
}


static exclude_flag_t
RecvDataFromSingleMap( BufferedIORead* bio,
		       int fdr,
		       Buffer *map,
		       void* tempbuf, 
		       int tempbufsize) {
    exclude_flag_t excl_flag;
    int items_count;
    int bytes=0;

    /*read last data flag 0 | 1, if reducer receives 1 then it should
     * exclude sender map node from communications*/
    bytes += bio->read( bio, fdr, &excl_flag, sizeof(int) );
    /*read items count*/
    bytes += bio->read( bio, fdr, &items_count, sizeof(int));
    WRITE_FMT_LOG( "readmap exclude flag=%d, items_count=%d\n", excl_flag, items_count );

    if ( items_count > 0 ){
	/*alloc memory for all array cells expected to receive*/
	AllocBuffer(map, MRITEM_SIZE, items_count);

	/*read items_count items*/
	ElasticBufItemData* item;
	for( int i=0; i < items_count; i++ ){
	    /*add item manually, no excesive copy doing*/
	    AddBufferItemVirtually(map);
	    item = (ElasticBufItemData*)BufferItemPointer( map, i );
	    /*save directly into array*/
	    bytes+=BufferedReadSingleMrItem( bio, fdr, item );
	}
    }
    WRITE_FMT_LOG( "readed %d bytes from Map node, fdr=%d, item count=%d\n", 
		   bytes, fdr, map->header.count );
    return excl_flag;
}


int 
ReduceNodeMain( struct MapReduceUserIf *mrif, 
		struct ChannelsConfigInterface *chif ){
    WRITE_LOG("ReduceNodeMain\n");
    assert(mrif);
    assert(mrif->Map);
    assert(mrif->Combine);
    assert(mrif->Reduce);
    assert(mrif->HashComparator);
#ifndef DISABLE_WRITE_LOG_BUFFER
    assert(mrif->HashAsString);
#endif

    __mrif = mrif;

    /*get map_nodes_count*/
    int *map_nodes_list = NULL;
    int map_nodes_count = chif->GetNodesListByType( chif, EMapNode, &map_nodes_list);
    int merge_buffers_count = map_nodes_count+1;

    /*Buffer for merge*/
    Buffer merged; memset( &merged, '\0', sizeof(merged) );
    /*Buffer for sorted*/
    Buffer all; memset( &all, '\0', sizeof(all) );
    /*recv Buffers to store received data from Map nodes*/
    Buffer recv[merge_buffers_count]; memset( recv, '\0', sizeof(recv) );

    int excluded_map_nodes[map_nodes_count];
    memset( excluded_map_nodes, '\0', sizeof(excluded_map_nodes) );

    /*merge arrays of buffers*/
    int last_merge_item_index = map_nodes_count;

    /*Declare pretty big buffer to provide efficient buffered IO, 
      free it at the function end*/
    void *recv_buffer = malloc(TEMP_BUFFER_SIZE);
    BufferedIORead* bio = AllocBufferedIORead( recv_buffer, TEMP_BUFFER_SIZE);

    /*read data from map nodes*/
    int leave_map_nodes; /*is used as condition for do while loop*/
    do{
	leave_map_nodes = 0;
	for( int i=0; i < map_nodes_count; i++ ){
	    /*If expecting data from current map node*/
	    if ( excluded_map_nodes[i] != MAP_NODE_EXCLUDE ){
		struct UserChannel *channel 
		    = chif->Channel(chif, EMapNode, map_nodes_list[i], EChannelModeRead );
		assert(channel);
		WRITE_FMT_LOG( "Read [%d]map#%d, fdr=%d\n", 
			       i, map_nodes_list[i], channel->fd );

		excluded_map_nodes[i] 
		    = RecvDataFromSingleMap( bio,
					     channel->fd, 
					     &recv[i], 
					     recv_buffer,
					     TEMP_BUFFER_SIZE );

		/*set next wait loop condition*/
		if ( excluded_map_nodes[i] != MAP_NODE_EXCLUDE ){
		    /* have mappers expected to send again*/
		    leave_map_nodes = 1;
		}
	    }
	    else{
		/*current map node excluded, free memory and reset data for unused buffers*/
		FreeBufferData(&recv[i]);
	    }
	}//for

	/*portion of data received from map nodes; merge all received data stored in
	 * recv_keys, recv_values arrays in positions range "[0, map_nodes_cunt-1]";
	 * last_merge_item_index points to previous merge result stored in the same arrays */
	WRITE_LOG( "Data received from mappers, merge it" );

	/*current result "all" always copying into last received data before 
	 *next merge ownership is transfered  */
	recv[last_merge_item_index] = all;

	/*Alloc buffers for merge results, and merge previous and new data*/
	int granularity = all.header.count>0? all.header.count/3 : 1000;
	int ret = AllocBuffer( &merged, MRITEM_SIZE, granularity );
	assert(!ret);
	ret = AllocBuffer( &all, MRITEM_SIZE, granularity );
	assert(!ret);
	MergeBuffersToNew( &merged, recv, merge_buffers_count );
	WRITE_FMT_LOG( "merge complete, keys count %d\n", merged.header.count );

	/*Merge complete, free source recv buffers*/
	for ( int i=0; i < last_merge_item_index+1; i++ ){
	    FreeBufferData(&recv[i]);
	}
	/**********************************************************/
	/*combine data every time while it receives from map nodes*/
	if ( mrif->Combine ){
	    WRITE_LOG_BUFFER(merged);
	    WRITE_FMT_LOG( "keys count before Combine: %d\n", 
			   (int)merged.header.count );
	    mrif->Combine( &merged, &all );
	    FreeBufferData( &merged );
	    WRITE_FMT_LOG( "keys count after Combine: %d\n", (int)all.header.count );
	    WRITE_LOG_BUFFER(all);
	}else{
	    int not_supported_yet = 0;
	    assert(not_supported_yet);
	}
	WRITE_FMT_LOG("sbrk()=%d\n", sbrk(0) );
    }while( leave_map_nodes != 0 );

    /*free intermediate keys,values buffers*/
    for(int i=0; i < merge_buffers_count; i++){
	FreeBufferData(&recv[i]);
    }

    if( mrif->Reduce ){
	/*user should output data into output file/s*/
	WRITE_FMT_LOG( "Reduce : %d items\n", (int)all.header.count );

	mrif->Reduce( &all );
    }

    free(map_nodes_list);
    free(bio);
    free(recv_buffer);
    FreeBufferData(&all);

    WRITE_LOG("ReduceNodeMain Complete\n");
    return 0;
}

