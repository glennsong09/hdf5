/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Programmer:  Neil Fortner
 *              March 10, 2014
 *
 * Purpose:     Test H5Dwrite_multi() and H5Dread_multi using randomized
 *              parameters in parallel.  Also tests H5Dwrite() and H5Dread()
 *              using a similar method.
 *
 *              Note that this test currently relies on all processes generating
 *              the same sequence of random numbers after using a shared seed
 *              value, therefore it may not work across multiple machines.
 */

#include "h5test.h"
#include "testpar.h"

#define T_PMD_ERROR                                                                                          \
    {                                                                                                        \
        nerrors++;                                                                                           \
        H5_FAILED();                                                                                         \
        AT();                                                                                                \
        printf("seed = %u\n", seed);                                                                         \
    }

#define FILENAME          "pmulti_dset.h5"
#define MAX_DSETS         5
#define MAX_DSET_X        15
#define MAX_DSET_Y        10
#define MAX_CHUNK_X       8
#define MAX_CHUNK_Y       6
#define MAX_HS_X          4
#define MAX_HS_Y          2
#define MAX_HS            2
#define MAX_POINTS        6
#define MAX_SEL_RETRIES   10
#define OPS_PER_FILE      25
#define DSET_MAX_NAME_LEN 8

/* Option flags */
#define MDSET_FLAG_CHUNK      0x01u
#define MDSET_FLAG_SHAPESAME  0x02u
#define MDSET_FLAG_MDSET      0x04u
#define MDSET_FLAG_COLLECTIVE 0x08u
#define MDSET_ALL_FLAGS       (MDSET_FLAG_CHUNK | MDSET_FLAG_SHAPESAME | MDSET_FLAG_MDSET | MDSET_FLAG_COLLECTIVE)

/* MPI variables */
int mpi_size;
int mpi_rank;

/* Names for datasets */
char dset_name[MAX_DSETS][DSET_MAX_NAME_LEN];

/* Random number seed */
unsigned seed;

/* Number of errors */
int nerrors = 0;

/*-------------------------------------------------------------------------
 * Function:    test_pmdset
 *
 * Purpose:     Test randomized I/O using one or more datasets.  Creates a
 *              file, runs OPS_PER_FILE read or write operations verifying
 *              that reads return the expected data, then closes the file.
 *              Runs the test with a new file niter times.
 *
 *              The operations can use either hyperslab or point
 *              selections.  Options are available for chunked or
 *              contiguous layout, use of multiple datasets and H5D*_multi
 *              calls, and use of the "shapesame" algorithm code path.  To
 *              avoid the shapesame path when that option is not set, this
 *              function simply adds a dimension to the memory buffer in a
 *              way that the shapesame code is not designed to handle.
 *
 * Return:      Number of errors
 *
 * Programmer:  Neil Fortner
 *              Monday, March 10, 2014
 *
 *-------------------------------------------------------------------------
 */
static void
test_pmdset(size_t niter, unsigned flags)
{
    H5D_rw_multi_t multi_info[MAX_DSETS];
    size_t         max_dsets;
    size_t         buf_size;
    size_t         ndsets;
    hid_t          file_id = -1;
    hid_t          fapl_id = -1;
    hid_t          dcpl_id = -1;
    hid_t          dxpl_id = -1;
    hsize_t        dset_dims[MAX_DSETS][3];
    hsize_t        chunk_dims[2];
    hsize_t        max_dims[2] = {H5S_UNLIMITED, H5S_UNLIMITED};
    unsigned *     rbuf        = NULL;
    unsigned *     rbufi[MAX_DSETS][MAX_DSET_X];
    unsigned *     erbuf = NULL;
    unsigned *     erbufi[MAX_DSETS][MAX_DSET_X];
    unsigned *     wbuf = NULL;
    unsigned *     wbufi[MAX_DSETS][MAX_DSET_X];
    unsigned *     efbuf = NULL;
    unsigned *     efbufi[MAX_DSETS][MAX_DSET_X];
    unsigned char *dset_usage;
    unsigned char *dset_usagei[MAX_DSETS][MAX_DSET_X];
    hbool_t        do_read;
    hbool_t        last_read;
    hbool_t        overlap;
    hsize_t        start[MAX_HS][3];
    hsize_t        count[MAX_HS][3];
    hsize_t        points[3 * MAX_POINTS];
    int            rank_data_diff;
    unsigned       op_data_incr;
    size_t         i, j, k, l, m, n, o, p;

    if (mpi_rank == 0)
        TESTING("random I/O");

    /* Calculate maximum number of datasets */
    max_dsets = (flags & MDSET_FLAG_MDSET) ? MAX_DSETS : 1;

    /* Calculate data increment per write operation */
    op_data_incr = (unsigned)max_dsets * MAX_DSET_X * MAX_DSET_Y * (unsigned)mpi_size;

    /* Calculate buffer size */
    buf_size = max_dsets * MAX_DSET_X * MAX_DSET_Y * sizeof(unsigned);

    /* Allocate buffers */
    if (NULL == (rbuf = (unsigned *)HDmalloc(buf_size)))
        T_PMD_ERROR
    if (NULL == (erbuf = (unsigned *)HDmalloc(buf_size)))
        T_PMD_ERROR
    if (NULL == (wbuf = (unsigned *)HDmalloc(buf_size)))
        T_PMD_ERROR
    if (NULL == (efbuf = (unsigned *)HDmalloc(buf_size)))
        T_PMD_ERROR
    if (NULL == (dset_usage = (unsigned char *)HDmalloc(max_dsets * MAX_DSET_X * MAX_DSET_Y)))
        T_PMD_ERROR

    /* Initialize buffer indices */
    for (i = 0; i < max_dsets; i++)
        for (j = 0; j < MAX_DSET_X; j++) {
            rbufi[i][j]       = rbuf + (i * MAX_DSET_X * MAX_DSET_Y) + (j * MAX_DSET_Y);
            erbufi[i][j]      = erbuf + (i * MAX_DSET_X * MAX_DSET_Y) + (j * MAX_DSET_Y);
            wbufi[i][j]       = wbuf + (i * MAX_DSET_X * MAX_DSET_Y) + (j * MAX_DSET_Y);
            efbufi[i][j]      = efbuf + (i * MAX_DSET_X * MAX_DSET_Y) + (j * MAX_DSET_Y);
            dset_usagei[i][j] = dset_usage + (i * MAX_DSET_X * MAX_DSET_Y) + (j * MAX_DSET_Y);
        } /* end for */

    /* Initialize 3rd dimension information (for tricking library into using
     * non-"shapesame" code */
    for (i = 0; i < max_dsets; i++)
        dset_dims[i][2] = 1;
    for (i = 0; i < MAX_HS; i++) {
        start[i][2] = 0;
        count[i][2] = 1;
    } /* end for */

    /* Initialize multi_info */
    for (i = 0; i < max_dsets; i++) {
        multi_info[i].dset_id       = -1;
        multi_info[i].dset_space_id = -1;
        multi_info[i].mem_type_id   = H5T_NATIVE_UINT;
        multi_info[i].mem_space_id  = -1;
    } /* end for */

    /* Generate memory dataspace */
    dset_dims[0][0] = MAX_DSET_X;
    dset_dims[0][1] = MAX_DSET_Y;
    if ((multi_info[0].mem_space_id =
             H5Screate_simple((flags & MDSET_FLAG_SHAPESAME) ? 2 : 3, dset_dims[0], NULL)) < 0)
        T_PMD_ERROR
    for (i = 1; i < max_dsets; i++)
        if ((multi_info[i].mem_space_id = H5Scopy(multi_info[0].mem_space_id)) < 0)
            T_PMD_ERROR

    /* Create fapl */
    if ((fapl_id = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        T_PMD_ERROR

    /* Set MPIO file driver */
    if ((H5Pset_fapl_mpio(fapl_id, MPI_COMM_WORLD, MPI_INFO_NULL)) < 0)
        T_PMD_ERROR

    /* Create dcpl */
    if ((dcpl_id = H5Pcreate(H5P_DATASET_CREATE)) < 0)
        T_PMD_ERROR

    /* Set fill time to alloc, and alloc time to early (so we always know
     * what's in the file) */
    if (H5Pset_fill_time(dcpl_id, H5D_FILL_TIME_ALLOC) < 0)
        T_PMD_ERROR
    if (H5Pset_alloc_time(dcpl_id, H5D_ALLOC_TIME_EARLY) < 0)
        T_PMD_ERROR

    /* Create dxpl */
    if ((dxpl_id = H5Pcreate(H5P_DATASET_XFER)) < 0)
        T_PMD_ERROR

    /* Set collective or independent I/O */
    if (flags & MDSET_FLAG_COLLECTIVE) {
        if (H5Pset_dxpl_mpio(dxpl_id, H5FD_MPIO_COLLECTIVE) < 0)
            T_PMD_ERROR
    } /* end if */
    else if (H5Pset_dxpl_mpio(dxpl_id, H5FD_MPIO_INDEPENDENT) < 0)
        T_PMD_ERROR

    for (i = 0; i < niter; i++) {
        /* Determine number of datasets */
        ndsets = (flags & MDSET_FLAG_MDSET) ? (size_t)((size_t)HDrandom() % max_dsets) + 1 : 1;

        /* Create file */
        if ((file_id = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl_id)) < 0)
            T_PMD_ERROR

        /* Create datasets */
        for (j = 0; j < ndsets; j++) {
            /* Generate file dataspace */
            dset_dims[j][0] = (hsize_t)((HDrandom() % MAX_DSET_X) + 1);
            dset_dims[j][1] = (hsize_t)((HDrandom() % MAX_DSET_Y) + 1);
            if ((multi_info[j].dset_space_id =
                     H5Screate_simple(2, dset_dims[j], (flags & MDSET_FLAG_CHUNK) ? max_dims : NULL)) < 0)
                T_PMD_ERROR

            /* Generate chunk (if requested) */
            if (flags & MDSET_FLAG_CHUNK) {
                chunk_dims[0] = (hsize_t)((HDrandom() % MAX_CHUNK_X) + 1);
                chunk_dims[1] = (hsize_t)((HDrandom() % MAX_CHUNK_Y) + 1);
                if (H5Pset_chunk(dcpl_id, 2, chunk_dims) < 0)
                    T_PMD_ERROR
            } /* end if */

            /* Create dataset */
            if ((multi_info[j].dset_id =
                     H5Dcreate2(file_id, dset_name[j], H5T_NATIVE_UINT, multi_info[j].dset_space_id,
                                H5P_DEFAULT, dcpl_id, H5P_DEFAULT)) < 0)
                T_PMD_ERROR
        } /* end for */

        /* Initialize read buffer and expected read buffer */
        (void)HDmemset(rbuf, 0, buf_size);
        (void)HDmemset(erbuf, 0, buf_size);

        /* Initialize write buffer */
        for (j = 0; j < max_dsets; j++)
            for (k = 0; k < MAX_DSET_X; k++)
                for (l = 0; l < MAX_DSET_Y; l++)
                    wbufi[j][k][l] = (unsigned)(((unsigned)mpi_rank * max_dsets * MAX_DSET_X * MAX_DSET_Y) +
                                                (j * MAX_DSET_X * MAX_DSET_Y) + (k * MAX_DSET_Y) + l);

        /* Initialize expected file buffer */
        (void)HDmemset(efbuf, 0, buf_size);

        /* Set last_read to TRUE so we don't reopen the file on the first
         * iteration */
        last_read = TRUE;

        /* Perform read/write operations */
        for (j = 0; j < OPS_PER_FILE; j++) {
            /* Decide whether to read or write */
            do_read = (hbool_t)(HDrandom() % 2);

            /* Barrier to ensure processes have finished the previous operation
             */
            MPI_Barrier(MPI_COMM_WORLD);

            /* If the last operation was a write we must close and reopen the
             * file to ensure consistency */
            /* Possibly change to MPI_FILE_SYNC at some point? -NAF */
            if (!last_read) {
                /* Close datasets */
                for (k = 0; k < ndsets; k++) {
                    if (H5Dclose(multi_info[k].dset_id) < 0)
                        T_PMD_ERROR
                    multi_info[k].dset_id = -1;
                } /* end for */

                /* Close file */
                if (H5Fclose(file_id) < 0)
                    T_PMD_ERROR
                file_id = -1;

                /* Barrier */
                MPI_Barrier(MPI_COMM_WORLD);

                /* Reopen file */
                if ((file_id = H5Fopen(FILENAME, H5F_ACC_RDWR, fapl_id)) < 0)
                    T_PMD_ERROR

                /* Reopen datasets */
                for (k = 0; k < ndsets; k++) {
                    if ((multi_info[k].dset_id = H5Dopen2(file_id, dset_name[k], H5P_DEFAULT)) < 0)
                        T_PMD_ERROR
                } /* end for */

                /* Barrier */
                MPI_Barrier(MPI_COMM_WORLD);
            } /* end if */

            /* Keep track of whether the last operation was a read */
            last_read = do_read;

            /* Loop over datasets */
            for (k = 0; k < ndsets; k++) {
                /* Reset selection */
                if (H5Sselect_none(multi_info[k].mem_space_id) < 0)
                    T_PMD_ERROR
                if (H5Sselect_none(multi_info[k].dset_space_id) < 0)
                    T_PMD_ERROR

                /* Reset dataset usage array, if writing */
                if (!do_read)
                    HDmemset(dset_usage, 0, max_dsets * MAX_DSET_X * MAX_DSET_Y);

                /* Iterate over processes */
                for (l = 0; l < (size_t)mpi_size; l++) {
                    /* Calculate difference between data in process being
                     * iterated over and that in this process */
                    rank_data_diff =
                        (int)((unsigned)max_dsets * MAX_DSET_X * MAX_DSET_Y) * ((int)l - (int)mpi_rank);

                    /* Decide whether to do a hyperslab or point selection */
                    if (HDrandom() % 2) {
                        /* Hyperslab */
                        size_t nhs      = (size_t)((HDrandom() % MAX_HS) + 1); /* Number of hyperslabs */
                        size_t max_hs_x = (MAX_HS_X <= dset_dims[k][0])
                                              ? MAX_HS_X
                                              : dset_dims[k][0]; /* Determine maximum hyperslab size in X */
                        size_t max_hs_y = (MAX_HS_Y <= dset_dims[k][1])
                                              ? MAX_HS_Y
                                              : dset_dims[k][1]; /* Determine maximum hyperslab size in Y */

                        for (m = 0; m < nhs; m++) {
                            overlap = TRUE;
                            for (n = 0; overlap && (n < MAX_SEL_RETRIES); n++) {
                                /* Generate hyperslab */
                                count[m][0] = (hsize_t)(((hsize_t)HDrandom() % max_hs_x) + 1);
                                count[m][1] = (hsize_t)(((hsize_t)HDrandom() % max_hs_y) + 1);
                                start[m][0] = (count[m][0] == dset_dims[k][0])
                                                  ? 0
                                                  : (hsize_t)HDrandom() % (dset_dims[k][0] - count[m][0] + 1);
                                start[m][1] = (count[m][1] == dset_dims[k][1])
                                                  ? 0
                                                  : (hsize_t)HDrandom() % (dset_dims[k][1] - count[m][1] + 1);

                                /* If writing, check for overlap with other processes */
                                overlap = FALSE;
                                if (!do_read)
                                    for (o = start[m][0]; (o < (start[m][0] + count[m][0])) && !overlap; o++)
                                        for (p = start[m][1]; (p < (start[m][1] + count[m][1])) && !overlap;
                                             p++)
                                            if (dset_usagei[k][o][p])
                                                overlap = TRUE;
                            } /* end for */

                            /* If we did not find a non-overlapping hyperslab
                             * quit trying to generate new ones */
                            if (overlap) {
                                nhs = m;
                                break;
                            } /* end if */

                            /* Select hyperslab if this is the current process
                             */
                            if (l == (size_t)mpi_rank) {
                                if (H5Sselect_hyperslab(multi_info[k].mem_space_id, H5S_SELECT_OR, start[m],
                                                        NULL, count[m], NULL) < 0)
                                    T_PMD_ERROR
                                if (H5Sselect_hyperslab(multi_info[k].dset_space_id, H5S_SELECT_OR, start[m],
                                                        NULL, count[m], NULL) < 0)
                                    T_PMD_ERROR
                            } /* end if */

                            /* Update expected buffers */
                            if (do_read) {
                                if (l == (size_t)mpi_rank)
                                    for (n = start[m][0]; n < (start[m][0] + count[m][0]); n++)
                                        for (o = start[m][1]; o < (start[m][1] + count[m][1]); o++)
                                            erbufi[k][n][o] = efbufi[k][n][o];
                            } /* end if */
                            else
                                for (n = start[m][0]; n < (start[m][0] + count[m][0]); n++)
                                    for (o = start[m][1]; o < (start[m][1] + count[m][1]); o++)
                                        efbufi[k][n][o] = (unsigned)((int)wbufi[k][n][o] + rank_data_diff);
                        } /* end for */

                        /* Update dataset usage array if writing */
                        if (!do_read)
                            for (m = 0; m < nhs; m++)
                                for (n = start[m][0]; n < (start[m][0] + count[m][0]); n++)
                                    for (o = start[m][1]; o < (start[m][1] + count[m][1]); o++)
                                        dset_usagei[k][n][o] = (unsigned char)1;
                    } /* end if */
                    else {
                        /* Point selection */
                        size_t npoints =
                            (size_t)(((size_t)HDrandom() % MAX_POINTS) + 1); /* Number of points */

                        /* Generate points */
                        for (m = 0; m < npoints; m++) {
                            overlap = TRUE;
                            for (n = 0; overlap && (n < MAX_SEL_RETRIES); n++) {
                                /* Generate point */
                                points[2 * m]       = (unsigned)((hsize_t)HDrandom() % dset_dims[k][0]);
                                points[(2 * m) + 1] = (unsigned)((hsize_t)HDrandom() % dset_dims[k][1]);

                                /* If writing, check for overlap with other
                                 * processes */
                                overlap = FALSE;
                                if (!do_read)
                                    if (dset_usagei[k][points[2 * m]][points[(2 * m) + 1]])
                                        overlap = TRUE;
                            } /* end for */

                            /* If we did not find a non-overlapping point quit
                             * trying to generate new ones */
                            if (overlap) {
                                npoints = m;
                                break;
                            } /* end if */
                        }     /* end for */

                        /* Update dataset usage array if writing */
                        if (!do_read)
                            for (m = 0; m < npoints; m++)
                                dset_usagei[k][points[2 * m]][points[(2 * m) + 1]] = (unsigned char)1;

                        /* Select points in file if this is the current process
                         */
                        if ((l == (size_t)mpi_rank) && (npoints > 0))
                            if (H5Sselect_elements(multi_info[k].dset_space_id, H5S_SELECT_APPEND, npoints,
                                                   points) < 0)
                                T_PMD_ERROR

                        /* Update expected buffers */
                        if (do_read) {
                            if (l == (size_t)mpi_rank)
                                for (m = 0; m < npoints; m++)
                                    erbufi[k][points[2 * m]][points[(2 * m) + 1]] =
                                        efbufi[k][points[2 * m]][points[(2 * m) + 1]];
                        } /* end if */
                        else
                            for (m = 0; m < npoints; m++)
                                efbufi[k][points[2 * m]][points[(2 * m) + 1]] =
                                    (unsigned)((int)wbufi[k][points[2 * m]][points[(2 * m) + 1]] +
                                               rank_data_diff);

                        /* Select points in memory if this is the current
                         * process */
                        if ((l == (size_t)mpi_rank) && (npoints > 0)) {
                            /* Convert to 3D for memory selection, if not using
                             * "shapesame" */
                            if (!(flags & MDSET_FLAG_SHAPESAME)) {
                                for (m = npoints - 1; m > 0; m--) {
                                    points[(3 * m) + 2] = 0;
                                    points[(3 * m) + 1] = points[(2 * m) + 1];
                                    points[3 * m]       = points[2 * m];
                                } /* end for */
                                points[2] = 0;
                            } /* end if */

                            /* Select elements */
                            if (H5Sselect_elements(multi_info[k].mem_space_id, H5S_SELECT_APPEND, npoints,
                                                   points) < 0)
                                T_PMD_ERROR
                        } /* end if */
                    }     /* end else */
                }         /* end for */
            }             /* end for */

            /* Perform I/O */
            if (do_read) {
                if (flags & MDSET_FLAG_MDSET) {
                    /* Set buffers */
                    for (k = 0; k < ndsets; k++)
                        multi_info[k].u.rbuf = rbufi[k][0];

                    /* Read datasets */
                    if (H5Dread_multi(dxpl_id, ndsets, multi_info) < 0)
                        T_PMD_ERROR
                } /* end if */
                else
                    /* Read */
                    if (H5Dread(multi_info[0].dset_id, multi_info[0].mem_type_id, multi_info[0].mem_space_id,
                                multi_info[0].dset_space_id, dxpl_id, rbuf) < 0)
                    T_PMD_ERROR

                /* Verify data */
                if (0 != memcmp(rbuf, erbuf, buf_size))
                    T_PMD_ERROR
            } /* end if */
            else {
                if (flags & MDSET_FLAG_MDSET) {
                    /* Set buffers */
                    for (k = 0; k < ndsets; k++)
                        multi_info[k].u.wbuf = wbufi[k][0];

                    /* Write datasets */
                    if (H5Dwrite_multi(dxpl_id, ndsets, multi_info) < 0)
                        T_PMD_ERROR
                } /* end if */
                else
                    /* Write */
                    if (H5Dwrite(multi_info[0].dset_id, multi_info[0].mem_type_id, multi_info[0].mem_space_id,
                                 multi_info[0].dset_space_id, dxpl_id, wbuf) < 0)
                    T_PMD_ERROR

                /* Update wbuf */
                for (l = 0; l < max_dsets; l++)
                    for (m = 0; m < MAX_DSET_X; m++)
                        for (n = 0; n < MAX_DSET_Y; n++)
                            wbufi[l][m][n] += op_data_incr;
            } /* end else */
        }     /* end for */

        /* Close */
        for (j = 0; j < ndsets; j++) {
            if (H5Dclose(multi_info[j].dset_id) < 0)
                T_PMD_ERROR
            multi_info[j].dset_id = -1;
            if (H5Sclose(multi_info[j].dset_space_id) < 0)
                T_PMD_ERROR
            multi_info[j].dset_space_id = -1;
        } /* end for */
        if (H5Fclose(file_id) < 0)
            T_PMD_ERROR
        file_id = -1;
    } /* end for */

    /* Close */
    for (i = 0; i < max_dsets; i++) {
        if (H5Sclose(multi_info[i].mem_space_id) < 0)
            T_PMD_ERROR
        multi_info[i].mem_space_id = -1;
    } /* end for */
    if (H5Pclose(dxpl_id) < 0)
        T_PMD_ERROR
    dxpl_id = -1;
    if (H5Pclose(dcpl_id) < 0)
        T_PMD_ERROR
    dcpl_id = -1;
    if (H5Pclose(fapl_id) < 0)
        T_PMD_ERROR
    fapl_id = -1;
    free(rbuf);
    rbuf = NULL;
    free(erbuf);
    erbuf = NULL;
    free(wbuf);
    wbuf = NULL;
    free(efbuf);
    efbuf = NULL;
    free(dset_usage);
    dset_usage = NULL;

    if (mpi_rank == 0)
        PASSED();

    return;
} /* end test_mdset() */

/*-------------------------------------------------------------------------
 * Function:    main
 *
 * Purpose:     Runs all tests with all combinations of configuration
 *              flags.
 *
 * Return:      Success:        0
 *              Failue:         1
 *
 * Programmer:  Neil Fortner
 *              Monday, March 10, 2014
 *
 *-------------------------------------------------------------------------
 */
int
main(int argc, char *argv[])
{
    unsigned i;
    int      ret;

    h5_reset();

    /* Initialize MPI */
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

    /* Generate random number seed, if rank 0 */
    if (MAINPROCESS)
        seed = (unsigned)HDtime(NULL);

    /* Broadcast seed from rank 0 (other ranks will receive rank 0's seed) */
    if (MPI_SUCCESS != MPI_Bcast(&seed, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD))
        T_PMD_ERROR

    /* Seed random number generator with shared seed (so all ranks generate the
     * same sequence) */
    HDsrandom(seed);

    /* Fill dset_name array */
    for (i = 0; i < MAX_DSETS; i++) {
        if ((ret = snprintf(dset_name[i], DSET_MAX_NAME_LEN, "dset%u", i)) < 0)
            T_PMD_ERROR
        if (ret >= DSET_MAX_NAME_LEN)
            T_PMD_ERROR
    } /* end for */

    for (i = 0; i <= MDSET_ALL_FLAGS; i++) {
        /* Print flag configuration */
        if (MAINPROCESS) {
            puts("\nConfiguration:");
            printf("  Layout:     %s\n", (i & MDSET_FLAG_CHUNK) ? "Chunked" : "Contiguous");
            printf("  Shape same: %s\n", (i & MDSET_FLAG_SHAPESAME) ? "Yes" : "No");
            printf("  I/O type:   %s\n", (i & MDSET_FLAG_MDSET) ? "Multi" : "Single");
            printf("  MPI I/O type: %s\n", (i & MDSET_FLAG_COLLECTIVE) ? "Collective" : "Independent");
        } /* end if */

        test_pmdset(10, i);
    } /* end for */

    /* Barrier to make sure all ranks are done before deleting the file, and
     * also to clean up output (make sure PASSED is printed before any of the
     * following messages) */
    if (MPI_SUCCESS != MPI_Barrier(MPI_COMM_WORLD))
        T_PMD_ERROR

    /* Delete file */
    if (mpi_rank == 0)
        if (MPI_SUCCESS != MPI_File_delete(FILENAME, MPI_INFO_NULL))
            T_PMD_ERROR

    /* Gather errors from all processes */
    MPI_Allreduce(&nerrors, &ret, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    nerrors = ret;

    if (MAINPROCESS) {
        printf("===================================\n");
        if (nerrors)
            printf("***Parallel multi dataset tests detected %d errors***\n", nerrors);
        else
            printf("Parallel multi dataset tests finished with no errors\n");
        printf("===================================\n");
    } /* end if */

    /* close HDF5 library */
    H5close();

    /* MPI_Finalize must be called AFTER H5close which may use MPI calls */
    MPI_Finalize();

    /* cannot just return (nerrors) because exit code is limited to 1 byte */
    return (nerrors != 0);
} /* end main() */