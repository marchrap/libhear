#include <unordered_map>
#include <vector>
#include <random>
#include <iostream>
#include <limits>
#include <functional>
#include <cassert>
#include <cstring>

#include <mpi.h>

#ifdef TSC_PROF
#include "tsc_x86.hpp"
#define TSC_NUM_MEASUREMENTS 10000000
#define TSC_WARMUP_CUTOFF 200 /* similar to OSU benchmarks */
#endif

#include "encrypt.hpp"
#include "hear.hpp"

/*
 * We need at least two pre-allocated buffers to enable pipelining,
 * i.e., memcopy + de-/en-cryption overlapped with communication.
 */

#ifdef USE_MPOOL
#include "mpool.hpp"
size_t mpool_size = 4;
size_t mpool_sbuf_len = 8388608;
#endif

#ifdef USE_PIPELINING
int pipelining_block_size = 65536;
#endif

const int root_rank = 0;

struct HearState
{

private:

    std::vector<std::vector<unsigned int>> _k_s_storage;
    std::unordered_map<MPI_Comm, std::size_t> _k_s_map;

    std::vector<unsigned int> _k_n_storage;
    std::unordered_map<MPI_Comm, std::size_t> _k_n_map;

    /* MPI_INT + MPI_SUM */
    std::function<void(unsigned int *, const unsigned int *, int, int, std::vector<unsigned int> &, unsigned int, bool)> encrypt_block_int_sum;
    std::function<void(unsigned int *, int, std::vector<unsigned int> &, unsigned int)> decrypt_block_int_sum;

    /* MPI_INT + MPI_PROD */
    std::function<void(unsigned int *, const unsigned int *, int, int, std::vector<unsigned int> &, unsigned int, bool)> encrypt_block_int_prod;
    std::function<void(unsigned int *, int, std::vector<unsigned int> &, unsigned int)> decrypt_block_int_prod;

    /* MPI_FLOAT + MPI_SUM*/
    std::function<void(float *, const float *, int, int, std::vector<unsigned int> &, unsigned int)> encrypt_block_float_sum;
    std::function<void(float *, int, std::vector<unsigned int> &, unsigned int)> decrypt_block_float_sum;
    std::function<unsigned int(unsigned int)> prng;

#ifdef USE_MPOOL
    mpool::SbufMpool _sbuf_mpool;
#endif

public:

    HearState(
#ifdef USE_MPOOL
	      std::size_t mpool_size, std::size_t mpool_sbuf_len
#endif
	      );
    ~HearState();

    void release_memory(void *buf);
    int insert_new_comm(MPI_Comm comm);
    void update_k_n(MPI_Comm comm);
    void* encrypt_sendbuf(const void *sendbuf, void *recvbuf, int count,
                          MPI_Datatype datatype, MPI_Op op, MPI_Comm comm);
    int decrypt_recvbuf(void *recvbuf, int count, MPI_Datatype datatype,
                        MPI_Op op, MPI_Comm comm);

#ifdef TSC_PROF
    std::vector<myInt64> tsc_comm;
    std::vector<myInt64> tsc_mmalloc;
    std::vector<myInt64> tsc_mfree;
    std::vector<myInt64> tsc_encrypt;
    std::vector<myInt64> tsc_decrypt;
#endif
};

class HearState *hear;


HearState::HearState(
#ifdef USE_MPOOL
		     std::size_t mpool_size,
		     std::size_t mpool_sbuf_len
#endif
		     )
#ifdef USE_MPOOL
    : _sbuf_mpool(mpool_size, mpool_sbuf_len)
#endif
{
    this->encrypt_block_int_sum = encryption::encrypt_int_sum_naive;
    this->decrypt_block_int_sum = encryption::decrypt_int_sum_naive;
    this->encrypt_block_float_sum = encryption::encrypt_float_sum_naive;
    this->decrypt_block_float_sum = encryption::decrypt_float_sum_naive;
    this->encrypt_block_int_prod = encryption::encrypt_int_prod_naive;
    this->decrypt_block_int_prod = encryption::decrypt_int_prod_naive;
    this->prng = encryption::prng_uint;

#ifdef AESNI
    if (const char* env = std::getenv("HEAR_ENABLE_AESNI")) {
        this->encrypt_block_int_sum = encryption::encrypt_int_sum_aesni128;
	this->decrypt_block_int_sum = encryption::decrypt_int_sum_aesni128;
	this->encrypt_block_float_sum = encryption::encrypt_float_sum_aesni128_unroll;
	this->decrypt_block_float_sum = encryption::decrypt_float_sum_aesni128_unroll;
	this->prng = encryption::aesni128_prng;
    }
#endif

#ifdef TSC_PROF
    init_tsc();
    tsc_comm.reserve(TSC_NUM_MEASUREMENTS);
    tsc_mmalloc.reserve(TSC_NUM_MEASUREMENTS);
    tsc_mfree.reserve(TSC_NUM_MEASUREMENTS);
    tsc_encrypt.reserve(TSC_NUM_MEASUREMENTS);
    tsc_decrypt.reserve(TSC_NUM_MEASUREMENTS);
#endif
}

#ifdef TSC_PROF
static myInt64 get_tsc_avg(std::vector<myInt64> &measurements, int comm_size)
{
    myInt64 sum = 0;
    int counter = 0;

    for (auto & val: measurements) {
	if (counter++ < TSC_WARMUP_CUTOFF) {
	    continue;
	}
	sum += val / comm_size;
    }

    return sum / (measurements.size() - TSC_WARMUP_CUTOFF);
}
#endif

HearState::~HearState()
{
#ifdef TSC_PROF
    int my_rank, comm_size;

    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    PMPI_Allreduce(MPI_IN_PLACE, tsc_comm.data(), tsc_comm.size(), MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (my_rank == 0) {
	std::cout << "comm=" << get_tsc_avg(tsc_comm, comm_size) << std::endl;
    }
#ifndef ALLREDUCE_BASELINE
    PMPI_Allreduce(MPI_IN_PLACE, tsc_mmalloc.data(), tsc_mmalloc.size(), MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    PMPI_Allreduce(MPI_IN_PLACE, tsc_mfree.data(), tsc_mfree.size(), MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    PMPI_Allreduce(MPI_IN_PLACE, tsc_encrypt.data(), tsc_encrypt.size(), MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    PMPI_Allreduce(MPI_IN_PLACE, tsc_decrypt.data(), tsc_decrypt.size(), MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (my_rank == 0) {
	std::cout << "mmalloc=" << get_tsc_avg(tsc_mmalloc, comm_size) << std::endl;
	std::cout << "mfree=" << get_tsc_avg(tsc_mfree, comm_size) << std::endl;
	std::cout << "encrypt=" << get_tsc_avg(tsc_encrypt, comm_size) << std::endl;
	std::cout << "decrypt=" << get_tsc_avg(tsc_decrypt, comm_size) << std::endl;
    }
#endif

#endif
}

inline void HearState::update_k_n(MPI_Comm comm)
{
    unsigned int tmp = this->prng(_k_n_storage[_k_n_map[comm]]);
    _k_n_storage[_k_n_map[comm]] = tmp;
}

inline int HearState::insert_new_comm(MPI_Comm comm)
{
    int comm_size;
    int my_rank;
    int ret;
    bool ok;

    MPI_Comm_size(comm, &comm_size);
    MPI_Comm_rank(comm, &my_rank);

    _k_s_storage.push_back(std::vector<unsigned int>(comm_size, my_rank));
    _k_s_storage.back()[my_rank] = static_cast<unsigned int>(encryption::encr_noise_generator());
    ok = _k_s_map.insert({comm, _k_s_storage.size() - 1}).second;
    assert(ok);
    ret = PMPI_Allgather(MPI_IN_PLACE, 1, MPI_UNSIGNED, _k_s_storage[_k_s_map[comm]].data(), 1, MPI_UNSIGNED, comm);
    if (ret != MPI_SUCCESS)
        return ret;

    assert(ret == MPI_SUCCESS);

    _k_n_storage.push_back(my_rank == root_rank ?
				 static_cast<unsigned int>(encryption::encr_noise_generator()) : 42);
    ok = _k_n_map.insert({comm, _k_n_storage.size() - 1}).second;
    assert(ok);

    ret = PMPI_Bcast(&_k_n_storage[_k_n_map[comm]], 1, MPI_UNSIGNED, root_rank, comm);
    if (ret != MPI_SUCCESS)
        return ret;

    assert(ret == MPI_SUCCESS);

    return MPI_SUCCESS;
}

inline void* HearState::encrypt_sendbuf(const void *sendbuf, void *recvbuf, int count,
                                        MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    void *encr_sbuf = nullptr;
    int type_size;
    int sbuf_len;
    int comm_size;
    int my_rank;

    MPI_Comm_size(comm, &comm_size);
    MPI_Comm_rank(comm, &my_rank);
    MPI_Type_size(datatype, &type_size);
    sbuf_len = count * type_size;

#ifdef TSC_PROF
    myInt64 t_mmalloc = start_tsc();
#endif

#ifndef USE_MPOOL
    encr_sbuf = new char[sbuf_len];
#else
    encr_sbuf = _sbuf_mpool.acquire_buf();
#endif
    if (encr_sbuf == nullptr)
        return nullptr;

#ifdef TSC_PROF
    hear->tsc_mmalloc.push_back(stop_tsc(t_mmalloc));
#endif

#ifdef TSC_PROF
    myInt64 t_encrypt = start_tsc();
#endif

    /* 3ncrypt10n */
    if (op == MPI_SUM) {
	if (datatype == MPI_INT) {
	    this->encrypt_block_int_sum(reinterpret_cast<unsigned int *>(encr_sbuf),
					reinterpret_cast<const unsigned int *>(sendbuf), count, my_rank,
					_k_s_storage[_k_s_map[comm]], _k_n_storage[_k_n_map[comm]],
					my_rank == (comm_size - 1) ? 1 : 0);

	} else if (datatype == MPI_FLOAT) {
	    this->encrypt_block_float_sum(reinterpret_cast<float *>(encr_sbuf),
					  reinterpret_cast<const float *>(sendbuf), count, my_rank,
					  _k_s_storage[_k_s_map[comm]], _k_n_storage[_k_n_map[comm]]);
	} else {
	    std::cerr << "Encryption for this MPI datatype is not supported!" << std::endl;
	    goto fail_cleanup;
	}
    } else if (op == MPI_PROD) {
	if (datatype == MPI_INT) {
	    this->encrypt_block_int_prod(reinterpret_cast<unsigned int *>(encr_sbuf),
					 reinterpret_cast<const unsigned int *>(sendbuf), count, my_rank,
					 _k_s_storage[_k_s_map[comm]], _k_n_storage[_k_n_map[comm]],
					 my_rank == (comm_size - 1) ? 1 : 0);
	} else {
	    std::cerr << "Encryption for this MPI datatype is not supported!" << std::endl;
	    goto fail_cleanup;
	}
    } else {
	std::cerr << "Encryption for this MPI op is not supported!" << std::endl;
	goto fail_cleanup;
    }

#ifdef TSC_PROF
    hear->tsc_encrypt.push_back(stop_tsc(t_encrypt));
#endif

    return encr_sbuf;

fail_cleanup:
#ifndef USE_MPOOL
    delete[] reinterpret_cast<char *>(encr_sbuf);
#else
    _sbuf_mpool.release_buf(encr_sbuf);
#endif
    return nullptr;
}

inline int HearState::decrypt_recvbuf(void *recvbuf, int count, MPI_Datatype datatype,
                                      MPI_Op op, MPI_Comm comm)
{
#ifdef TSC_PROF
    myInt64 t_decrypt = start_tsc();
#endif

    /* d3crypt10n */
    if (op == MPI_SUM) {
	if (datatype == MPI_INT) {
	    this->decrypt_block_int_sum(reinterpret_cast<unsigned int *>(recvbuf), count,
					_k_s_storage[_k_s_map[comm]], _k_n_storage[_k_n_map[comm]]);
	} else if (datatype == MPI_FLOAT) {
	    this->decrypt_block_float_sum(reinterpret_cast<float *>(recvbuf), count,
					  _k_s_storage[_k_s_map[comm]], _k_n_storage[_k_n_map[comm]]);
	} else {
	    std::cerr << "Decryption for this MPI datatype is not supported!" << std::endl;
	    return MPI_ERR_TYPE;
	}
    } else if (op == MPI_PROD) {
	if (datatype == MPI_INT) {
	    this->decrypt_block_int_prod(reinterpret_cast<unsigned int *>(recvbuf), count,
					 _k_s_storage[_k_s_map[comm]], _k_n_storage[_k_n_map[comm]]);
	} else {
	    std::cerr << "Decryption for this MPI datatype is not supported!" << std::endl;
	    return MPI_ERR_TYPE;
	}
    } else {
	std::cerr << "Decryption for this MPI op is not supported!" << std::endl;
	return MPI_ERR_TYPE;
    }

#ifdef TSC_PROF
    hear->tsc_decrypt.push_back(stop_tsc(t_decrypt));
#endif

    return MPI_SUCCESS;
}

inline void HearState::release_memory(void *buf)
{
#ifdef TSC_PROF
    myInt64 t_free = start_tsc();
#endif
    assert(buf);
#ifndef USE_MPOOL
    delete[] static_cast<char *>(buf);
#else
    _sbuf_mpool.release_buf(buf);
#endif
#ifdef TSC_PROF
    hear->tsc_mfree.push_back(stop_tsc(t_free));
#endif
}

/*
 * PMPI_* wrappers
 */

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
#ifdef USE_PIPELINING
    MPI_Request req;
    MPI_Status status;
    int prev_offset, cur_offset, next_offset;
    int prev_count, cur_count, next_count, total_count;
    void *encr_sendbuf_next;
#endif
    void *encr_sendbuf;
    int dtype_size;
    int ret;

#ifdef ALLREDUCE_BASELINE
#ifdef TSC_PROF
    myInt64 t_baseline = start_tsc();
#endif
    ret = PMPI_Allreduce(sendbuf, recvbuf, count, datatype, op, comm);
#ifdef TSC_PROF
    hear->tsc_comm.push_back(stop_tsc(t_baseline));
#endif
    return ret;
#endif

    if (((op != MPI_SUM) && (op != MPI_PROD)) || ((datatype != MPI_INT) && (datatype != MPI_FLOAT)))
            return PMPI_Allreduce(sendbuf, recvbuf, count, datatype, op, comm);

    MPI_Type_size(datatype, &dtype_size);

#ifdef DCHECK
    void *valid_rbuf = new char[dtype_size * count];
    assert(valid_rbuf);
    PMPI_Allreduce(sendbuf, valid_rbuf, count, datatype, op, comm);
#endif

#ifdef DEBUG
    std::cerr << "MPI_Alleduce() call interception" << std::endl;
#endif

    /*
     * Encryption logic starts here
     */

    hear->update_k_n(comm);

#ifndef USE_PIPELINING
    encr_sendbuf = hear->encrypt_sendbuf(sendbuf, recvbuf, count, datatype, op, comm);
    if (encr_sendbuf == nullptr)
        return MPI_ERR_BUFFER;

#ifdef TSC_PROF
    myInt64 t_comm = start_tsc();
#endif
    ret = PMPI_Allreduce(encr_sendbuf, recvbuf, count, datatype, op, comm);
    if (ret != MPI_SUCCESS)
        goto cleanup;
#ifdef TSC_PROF
    hear->tsc_comm.push_back(stop_tsc(t_comm));
#endif

    ret = hear->decrypt_recvbuf(recvbuf, count, datatype, op, comm);
    if (ret != MPI_SUCCESS)
        goto cleanup;

    hear->release_memory(encr_sendbuf);
#else
    /*
     * Overlap communication of n'th block with decryption of
     * n-1'th block and encryption of n+1'th block
     */
    total_count = count;
    cur_count = total_count < pipelining_block_size ? total_count : pipelining_block_size;
    prev_offset = cur_offset = next_offset = 0;

    encr_sendbuf = hear->encrypt_sendbuf(sendbuf, recvbuf, cur_count, datatype, op, comm);
    if (!encr_sendbuf) {
        ret = MPI_ERR_BUFFER;
        goto cleanup;
    }

    while (total_count) {
#ifdef TSC_PROF
	myInt64 t_comm = start_tsc();
#endif
        ret = PMPI_Iallreduce(encr_sendbuf, reinterpret_cast<char *>(recvbuf) + cur_offset, cur_count,
                              datatype, op, comm, &req);
        if (ret != MPI_SUCCESS)
            goto cleanup;

        if (cur_offset > 0) {
            ret = hear->decrypt_recvbuf(reinterpret_cast<char *>(recvbuf) + prev_offset, prev_count,
                                        datatype, op, comm);
            if (ret != MPI_SUCCESS) {
                goto cleanup;
            }
        }

        total_count -= cur_count;

        if (total_count) {
            next_offset += cur_count * dtype_size;
            next_count = total_count < pipelining_block_size ? total_count : pipelining_block_size;
            encr_sendbuf_next = hear->encrypt_sendbuf(reinterpret_cast<const char *>(sendbuf) + next_offset,
                                                      reinterpret_cast<char *>(recvbuf) + next_offset,
                                                      next_count, datatype, op, comm);
            if (!encr_sendbuf_next) {
                ret = MPI_ERR_BUFFER;
                goto cleanup;
            }
        }

        PMPI_Wait(&req, MPI_STATUS_IGNORE);
#ifdef TSC_PROF
	hear->tsc_comm.push_back(stop_tsc(t_comm));
#endif
        prev_count = cur_count;
        prev_offset = cur_offset;

        cur_count = next_count;
        cur_offset = next_offset;

        hear->release_memory(encr_sendbuf);
        encr_sendbuf = encr_sendbuf_next;
    }

    ret = hear->decrypt_recvbuf(reinterpret_cast<char *>(recvbuf) + prev_offset, prev_count, datatype, op, comm);
    if (ret != MPI_SUCCESS)
        return ret;
#endif

#ifdef DCHECK
    assert(!std::memcmp(valid_rbuf, recvbuf, dtype_size * count));
#endif

    return MPI_SUCCESS;

cleanup:
#ifdef DCHECK
    delete[] reinterpret_cast<char *>(valid_rbuf);
#endif
    hear->release_memory(encr_sendbuf);
    return ret;
}

static void alloc_state()
{

#ifdef USE_PIPELINING
    if (const char* env = std::getenv("HEAR_PIPELINING_BLOCK_SIZE"))
        pipelining_block_size = std::atoi(env);
#endif

#ifdef USE_MPOOL
    if (const char* env = std::getenv("HEAR_MPOOL_SIZE"))
        mpool_size = std::atoi(env);

    if (const char* env = std::getenv("HEAR_MPOOL_SBUF_LEN"))
        mpool_sbuf_len = std::atoi(env);

    hear = new HearState(mpool_size, mpool_sbuf_len);
    assert(hear);
#else
    hear = new HearState();
    assert(hear);
#endif

#ifdef AESNI
    char encr_key[] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    encryption::aesni128_load_key(encr_key);
#endif
}

int MPI_Init(int *argc, char ***argv)
{
    int ret;

#ifdef DEBUG
    std::cerr << "MPI_Init() call interception" << std::endl;
#endif
    ret = PMPI_Init(argc, argv);

    alloc_state();

    if (ret == MPI_SUCCESS)
        ret = hear->insert_new_comm(MPI_COMM_WORLD);

    return ret;
}

int MPI_Init_thread(int *argc, char ***argv, int required, int *provided)
{
    int ret;

#ifdef DEBUG
    std::cerr << "MPI_Init_thread() call interception" << std::endl;
#endif
    ret = PMPI_Init_thread(argc, argv, required, provided);

    alloc_state();

    if (ret == MPI_SUCCESS)
        ret = hear->insert_new_comm(MPI_COMM_WORLD);

    return ret;
}

int MPI_Comm_create(MPI_Comm comm, MPI_Group group, MPI_Comm *newcomm)
{
    int ret;

#ifdef DEBUG
    std::cerr << "MPI_Comm_create() call interception" << std::endl;
#endif
    ret = PMPI_Comm_create(comm, group, newcomm);
    if (ret == MPI_SUCCESS)
        ret = hear->insert_new_comm(*newcomm);

    return ret;
}

int MPI_Comm_split(MPI_Comm comm, int color, int key, MPI_Comm *newcomm)
{
    int ret;

#ifdef DEBUG
    std::cerr << "MPI_Comm_split() call interception" << std::endl;
#endif
    ret = PMPI_Comm_split(comm, color, key, newcomm);
    if (ret == MPI_SUCCESS)
        ret = hear->insert_new_comm(*newcomm);

    return ret;
}

int MPI_Comm_dup(MPI_Comm comm, MPI_Comm * newcomm)
{
    int ret;

#ifdef DEBUG
    std::cerr << "MPI_Comm_dup() call interception" << std::endl;
#endif
    ret = PMPI_Comm_dup(comm, newcomm);
    if (ret == MPI_SUCCESS)
        ret = hear->insert_new_comm(*newcomm);

    return ret;
}

int MPI_Finalize()
{
#ifdef DEBUG
    std::cerr << "MPI_Finalize() call interception" << std::endl;
#endif
    delete hear;

    return PMPI_Finalize();
}
