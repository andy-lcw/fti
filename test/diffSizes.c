/**
 *  @file   diffSizes.c
 *  @author Karol Sierocinski (ksiero@man.poznan.pl)
 *  @date   March, 2017
 *  @brief  FTI testing program.
 *
 *  Testing FTI_Init, FTI_Checkpoint, FTI_Status, FTI_Recover, FTI_Finalize,
 *  saving last checkpoint to PFS
 *
 *  Every process in every iteration expand their array and set value to each
 *  index and make checkpoint. Every rank has different size of checkpoint.
 *
 *  Program don't end with FTI_Finalize to make sure that checkpoint files
 *  will stay local (for L1, L2 and L3).
 *
 *  First execution this program should be with fail flag = 1, because
 *  then FTI saves checkpoint and program stops after ITER_STOP iteration.
 *  Second execution must be with the same #defines and flag = 0 to
 *  properly recover data. It is important that FTI config file got
 *  keep_last_ckpt = 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <fti.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../deps/iniparser/iniparser.h"
#include "../deps/iniparser/dictionary.h"

#define ITERATIONS 111          //iterations
#define ITER_CHECK 10           //every ITER_CHECK iterations make checkpoint
#define ITER_STOP 63            //stop work after ITER_STOP iterations

#define WORK_DONE 0
#define VERIFY_FAILED 1
#define CHECKPOINT_FAILED 2
#define RECOVERY_FAILED 3

#define VERIFY_SUCCESS 0

#define INIT_SIZE 64

/*-------------------------------------------------------------------------*/
/**
    Verifies final result.
 **/
/*-------------------------------------------------------------------------*/
int verify(long* array, int world_rank)
{
    int i;
    int size = world_rank * ITERATIONS;
    for (i = 0; i < size; i++) {
        if (array[i] != world_rank * ITERATIONS) {
            printf("array[%d] = %ld, should be %d.\n", i, array[i], world_rank * ITERATIONS);
            return VERIFY_FAILED;
        }
    }
    return VERIFY_SUCCESS;
}

/*-------------------------------------------------------------------------*/
/**
    @brief      Do work to makes checkpoints
    @param      world_rank          FTI_COMM rank
    @param      world_size          FTI_COMM size
    @param      checkpoint_level    Checkpont level to all checkpoints
    @param      fail                True if stop after ITER_STOP, false if resuming work
    @return     integer             WORK_DONE if successful.
 **/
/*-------------------------------------------------------------------------*/
int do_work(int world_rank, int world_size, int checkpoint_level, int fail)
{
    int res;
    int i = 0, j;
    int size = (world_rank + 1) * INIT_SIZE;
    int originSize = size;
    int addToSize = world_rank;

    FTI_Protect(0, &i, 1, FTI_INTG);
    FTI_Protect(1, &size, 1, FTI_INTG);
    long* buf = malloc (sizeof(long) * size);
    for (j = 0; j < size; j++) {
        buf[j] = 0;
    }
    FTI_Protect(2, buf, size, FTI_LONG);
    //checking if this is recovery run
    if (FTI_Status() != 0 && fail == 0) {
        /* when we add FTI_Realloc();
        buf = FTI_Realloc(2, buf);
        if (buf == NULL) {
            printf("%d: Reallocation failed!\n", world_rank);
            return RECOVERY_FAILED;
        }
        */

        //need to call FTI_Recover twice to get all data
        res = FTI_Recover();
        buf = realloc (buf, sizeof(long) * size);
        FTI_Protect(2, buf, size, FTI_LONG);
        res = FTI_Recover();
        if (res != 0) {
            printf("%d: Recovery failed! FTI_Recover returned %d.\n", world_rank, res);
            return RECOVERY_FAILED;
        }
    }
    //if recovery, but recover values don't match
    if (fail == 0) {
        int expectedI = ITER_STOP - ITER_STOP % ITER_CHECK;
        if (i != expectedI){
            printf("%d: i = %d, should be %d\n", world_rank, i, expectedI);
            return RECOVERY_FAILED;
        }
        int expectedSize = originSize + (i * addToSize);
        if (size != expectedSize) {
            printf("%d: size = %d, should be %d\n", world_rank, size, expectedSize);
            return RECOVERY_FAILED;
        }
        int recoverySize = 2 * sizeof(int); //i and size
        /* when we add FTI_Realloc();
        recoverySize += 3 * sizeof(long); //counts
        */
        for (j = 0; j < size; j++) {
            if (buf[j] != i * world_rank) {
                printf("%d: Recovery size = %d\n", world_rank, recoverySize);
                printf("%d: buf[%d] = %ld, should be %d\n", world_rank, j, buf[j], i * world_rank);
                return RECOVERY_FAILED;
            }
            recoverySize += sizeof(long);
        }
        printf("%d: Recovery size = %d\n", world_rank, recoverySize);
    }
    if (world_rank == 0) {
        printf("Starting work at i = %d.\n", i);
    }
    for (; i < ITERATIONS; i++) {
        //checkpoint after every ITER_CHECK iterations
        if (i%ITER_CHECK == 0) {
            FTI_Protect(2, buf, size, FTI_LONG);
            res = FTI_Checkpoint(i/ITER_CHECK + 1, checkpoint_level);
            if (res != FTI_DONE) {
                printf("%d: Checkpoint failed! FTI_Checkpoint returned %d.\n", world_rank, res);
                return CHECKPOINT_FAILED;
            }
        }

        size += addToSize;                      //enlarge size
        buf = realloc (buf, sizeof(long) * size);
        long tempValue = buf[0];
        for (j = 0; j < size; j++) {
            buf[j] = tempValue + world_rank;
        }
        //stoping after ITER_STOP iterations
        if (fail && i >= ITER_STOP) {
            if (world_rank == 0) {
                printf("Work stopped at i = %d.\n", ITER_STOP);
            }
            return WORK_DONE;
        }
    }

    int rtn = verify(buf, world_rank);

    free(buf);

    return WORK_DONE;
}


int init(char** argv, int* checkpoint_level, int* fail, int* ckptIO)
{
    int rtn = 0;    //return value
    if (argv[1] == NULL) {
        printf("Missing first parameter (config file).\n");
        rtn = 1;
    }
    if (argv[2] == NULL) {
        printf("Missing second parameter (checkpoint level).\n");
        rtn = 1;
    }
    else {
        *checkpoint_level = atoi(argv[2]);
    }
    if (argv[3] == NULL) {
        printf("Missing third parameter (if fail).\n");
        rtn = 1;
    }
    else {
        *fail = atoi(argv[3]);
    }
    if (argv[4] == NULL) {
        printf("Missing fourth parameter (ckptIO set to 1).\n");
        *ckptIO = 1;
    }
    else {
        *ckptIO = atoi(argv[4]);
    }
    return rtn;
}


int checkFileSizes(int* mpi_ranks, int world_size, int global_world_size, int level, int fail)
{
    dictionary* ini = iniparser_load("config.fti");
    char* exec_id = malloc (sizeof(char) * 256);
    exec_id = iniparser_getstring(ini, "Restart:exec_id", NULL);
    int nodeSize = (int)iniparser_getint(ini, "Basic:node_size", -1);
    int ckptIO = (int)iniparser_getint(ini, "Basic:ckpt_io", -1);
    int nodes = nodeSize ? global_world_size / nodeSize : 0;
    int i;
    char str[256];
    char path[256];

    DIR *dir;
    struct dirent *ent;
    int j;
    for (j = 0; j < nodes; j++) {
        if (level == 4) {
            sprintf(path, "./Global/%s/l4", exec_id);
        } else {
            sprintf(path, "./Local/node%d/%s/l%d", j, exec_id, level);
        }
        if ((dir = opendir (path)) != NULL) {
            while ((ent = readdir (dir)) != NULL) {
                if (strstr(ent->d_name , "Rank") != NULL) {
                    sprintf(str, "%s/%s", path, ent->d_name);

                    FILE* f = fopen(str, "rb");
                    fseek(f, 0L, SEEK_END);
                    int fileSize = ftell(f);

                    //get rank from file name
                    int id, rank;
                    sscanf(ent->d_name, "Ckpt%d-Rank%d.fti", &id, &rank);
                    for (i = 0; i < world_size; i++) {
                        if (rank == mpi_ranks[i]) {
                            rank = i;
                            break;
                        }
                    }

                    int expectedSize = 0;
                    /* when we add FTI_Realloc();
                    expectedSize += sizeof(long) * 2; //(i and size) length
                    */
                    expectedSize += sizeof(int) * 2; //i and size

                    int lastCheckpointIter;
                    if (fail) {
                        lastCheckpointIter = ITER_STOP - ITER_STOP % ITER_CHECK;
                    }
                    else {
                        lastCheckpointIter = (ITERATIONS - 1) - (ITERATIONS - 1) % ITER_CHECK;
                    }

                    expectedSize += ((rank + 1) * INIT_SIZE + lastCheckpointIter * rank) * sizeof(long);

                    printf("%d: Last checkpoint file size = %d\n", rank, fileSize);
                    if (fileSize != expectedSize) {
                        printf("%d: Last checkpoint file size = %d, should be %d.\n", rank, fileSize, expectedSize);

                        fclose(f);
                        closedir (dir);

                        return 1;
                    }
                    fclose(f);
                }
                if (strstr(ent->d_name , "mpiio") != NULL) {
                    sprintf(str, "%s/%s", path, ent->d_name);
                    FILE* f = fopen(str, "rb");
                    fseek(f, 0L, SEEK_END);
                    int fileSize = ftell(f);
                    int expectedAllSizes = 0;
                    for (i = 0; i < world_size; i++) {
                        int expectedSize = sizeof(int) * 2; //i and size
                        int lastCheckpointIter;
                        if (fail) {
                            lastCheckpointIter = ITER_STOP - ITER_STOP % ITER_CHECK;
                        }
                        else {
                            lastCheckpointIter = (ITERATIONS - 1) - (ITERATIONS - 1) % ITER_CHECK;
                        }
                        expectedSize += ((i + 1) * INIT_SIZE + lastCheckpointIter * i) * sizeof(long);
                        expectedAllSizes += expectedSize;
                        printf("%d: Last checkpoint size = %d\n", i, expectedSize);
                    }
                    if (fileSize != expectedAllSizes) {
                        printf("Last checkpoint file size = %d, should be %d.\n", fileSize, expectedAllSizes);
                        fclose(f);
                        closedir (dir);

                        return 1;
                    }
                    fclose(f);
                }
            }
            closedir (dir);
        }
        else {
            //could not open directory
            perror ("Checking file size failed: ");
            free(exec_id);
            return 1;
        }
        if (level == 4) break;
    }
    free(exec_id);
    return 0;
}

/*-------------------------------------------------------------------------*/
/**
    @return     integer     0 if successful, 1 otherwise
 **/
/*-------------------------------------------------------------------------*/
int main(int argc, char** argv)
{
    int checkpoint_level, fail, ckptIO;
    char str[FTI_BUFS];
    dictionary* ini;
    if (init(argv, &checkpoint_level, &fail, &ckptIO)) return 1;   //verify args

    MPI_Init(&argc, &argv);
    int global_world_rank, global_world_size;                          //MPI_COMM rank
    MPI_Comm_rank(MPI_COMM_WORLD, &global_world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &global_world_size);

    if (global_world_rank == 0) {
        ini = iniparser_load("config.fti");
        sprintf(str, "%d", ckptIO);
        // Set failure to 'restart'
        iniparser_set(ini, "Basic:ckpt_io", str);
        FILE* fd = fopen("config.fti", "w");
        if (fd == NULL) {
            printf("Failed to open the configuration file.");
            iniparser_freedict(ini);
            return 1;
        }

        // Write new configuration
        iniparser_dump_ini(ini, fd);
        if (fclose(fd) != 0) {
            printf("Failed to close the configuration file.");
            iniparser_freedict(ini);
            return 1;
        }
        // Free dictionary
        iniparser_freedict(ini);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    FTI_Init(argv[1], MPI_COMM_WORLD);
    int world_rank, world_size;                     //FTI_COMM rank and size
    MPI_Comm_rank(FTI_COMM_WORLD, &world_rank);
    MPI_Comm_size(FTI_COMM_WORLD, &world_size);

    int rtn = do_work(world_rank, world_size, checkpoint_level, fail);

    //need MPI ranks to know checkpoint files names
    int* mpi_ranks = malloc (sizeof(int) * world_size);
    MPI_Gather(&global_world_rank, 1, MPI_INT, mpi_ranks, 1, MPI_INT, 0, FTI_COMM_WORLD);

    ini = iniparser_load("config.fti");
    int heads = (int)iniparser_getint(ini, "Basic:head", -1);
    int nodeSize = (int)iniparser_getint(ini, "Basic:node_size", -1);
    int tag = (int)iniparser_getint(ini, "Advanced:mpi_tag", -1);
    int res;
    if (checkpoint_level != 1) {
        int isInline = -1;
        int heads = (int)iniparser_getint(ini, "Basic:head", -1);
        switch (checkpoint_level) {
            case 2:
                isInline = (int)iniparser_getint(ini, "Basic:inline_l2", 1);
                break;
            case 3:
                isInline = (int)iniparser_getint(ini, "Basic:inline_l3", 1);
                break;
            case 4:
                isInline = (int)iniparser_getint(ini, "Basic:inline_l4", 1);
                break;
        }
        if (isInline == 0) {
            //waiting untill head do Post-checkpointing
            MPI_Recv(&res, 1, MPI_INT, global_world_rank - (global_world_rank%nodeSize) , tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
    }
    if (heads > 0) {
        res = FTI_ENDW;
        //sending END WORK to head to stop listening
        MPI_Send(&res, 1, MPI_INT, global_world_rank - (global_world_rank%nodeSize), tag, MPI_COMM_WORLD);
        //Barrier needed for heads (look FTI_Finalize() in api.c)
        MPI_Barrier(MPI_COMM_WORLD);
    }

    if (world_rank == 0 && !rtn) {
        rtn = checkFileSizes(mpi_ranks, world_size, global_world_size, checkpoint_level, fail);
        if (!rtn && !fail) {
            printf("Success.\n");
        }
    }
    MPI_Barrier(FTI_COMM_WORLD);
    //There is no FTI_Finalize(), because want to recover also from L1, L2, L3
    MPI_Finalize();

    free(mpi_ranks);

    return rtn;
}
