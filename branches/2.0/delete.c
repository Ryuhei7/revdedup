/**
 * @file 	delete.c
 * @brief	Implements bucket deletion with version tagged
 * @author	Ng Chun Ho
 */

#include <revdedup.h>
#include <queue.h>

typedef struct {
	SMEntry * en;
	uint8_t data[MAX_SEG_SIZE];
} SimpleSegment;

SMEntry * sen;
BMEntry * ben;

uint8_t padding[BLOCK_SIZE];

/**
 * Create a new bucket in memory
 * @return			Created bucket
 */
static Bucket * NewBucket() {
	char buf[64];
	Bucket * b = malloc(sizeof(Bucket));
	b->id = ++((BucketLog *) ben)->bucketID;
	b->sid = ((SegmentLog *) sen)->segID;
	b->segs = 0;
	b->size = 0;

	sprintf(buf, DATA_DIR "bucket/%08lx", b->id);
	b->fd = creat(buf, 0644);
	assert(b->fd != -1);
	return b;
}

/**
 * Seal the bucket on disk
 * @param b			Bucket to seal
 */
static void SaveBucket(Bucket * b) {
	ssize_t remain = (BLOCK_SIZE - (b->size % BLOCK_SIZE)) % BLOCK_SIZE;
	assert(write(b->fd, padding, remain) == remain);
	close(b->fd);

	ben[b->id].sid = b->sid;
	ben[b->id].segs = b->segs;
	ben[b->id].size = b->size + remain;
	ben[b->id].psize = 0;
	ben[b->id].ver = -1;			// Infinity
	free(b);
}

/**
 * Insert a segment into buckets
 * @param b			Bucket to insert
 * @param sseg		Segment to write
 * @return			Bucket for subsequent insertion
 */
static Bucket * BucketInsert(Bucket * b, SimpleSegment * sseg) {
	if (b == NULL) {
		b = NewBucket();
	}
	if (b->size + sseg->en->len > BUCKET_SIZE) {
		SaveBucket(b);
		b = NewBucket();
	}
	if (sseg->en - sen < b->sid) {
		b->sid = sseg->en - sen;
	}

	sseg->en->bucket = b->id;
	sseg->en->pos = b->size;

	assert(write(b->fd, sseg->data, sseg->en->len) == sseg->en->len);
	b->segs++;
	b->size += sseg->en->len;

	return b;
}

/**
 * Main loop for processing segments
 * @param ptr		useless
 */
static void * process(void * ptr) {
	Queue * iq = (Queue *)ptr;
	Bucket * b = NULL;
	SimpleSegment * sseg;
	memset(padding, 0, BLOCK_SIZE);
	while ((sseg = (SimpleSegment *)Dequeue(iq)) != NULL) {
		b = BucketInsert(b, sseg);
		free(sseg);
	}
	if (b != NULL) {
		SaveBucket(b);
	}
	return NULL;
}

int main(int argc, char * argv[]) {
	if (argc != 1) {
		fprintf(stderr, "Usage : %s\n", argv[0]);
		return 0;
	}
	char buf[128];
	int fd;
	fd = open(DATA_DIR "clog", O_RDWR);
	sen = MMAP_FD(fd, MAX_ENTRIES(sizeof(SMEntry)));
	close(fd);

	fd = open(DATA_DIR "blog", O_RDWR);
	ben = MMAP_FD(fd, MAX_ENTRIES(sizeof(BMEntry)));
	close(fd);

	Queue * cq = LongQueue();
	pthread_t wid, pid;
	pthread_create(&wid, NULL, process, cq);

	uint64_t i, j, ptr;
	uint64_t bucketID = ((BucketLog *)ben)->bucketID;
	for (i = 1; i <= bucketID; i++) {
		if (ben[i].psize == 0) {
			continue;
		}
		sprintf(buf, DATA_DIR "bucket/%08lx", i);
		fd = open(buf, O_RDONLY);
		posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
		for (j = 0, ptr = ben[i].sid; j < ben[i].segs; ptr++) {
			if (sen[ptr].bucket != i) {
				continue;
			}
			/// Rewrites segments with positive reference count
			if (sen[ptr].ref > 0) {
				SimpleSegment * sseg = malloc(sizeof(SimpleSegment));
				sseg->en = &sen[ptr];
				assert(pread(fd, sseg->data, sseg->en->len, sseg->en->pos) == sseg->en->len);
				Enqueue(cq, sseg);
			}
			j++;
		}
		close(fd);
		unlink(buf);
		memset(ben + i, 0, sizeof(BMEntry));
	}

	Enqueue(cq, NULL);
	pthread_join(wid, NULL);
	DelQueue(cq);
	sync();

	munmap(ben, MAX_ENTRIES(sizeof(BMEntry)));
	munmap(sen, MAX_ENTRIES(sizeof(SMEntry)));

	return 0;
}
