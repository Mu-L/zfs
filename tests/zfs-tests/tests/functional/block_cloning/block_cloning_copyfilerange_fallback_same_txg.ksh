#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2023, Klara Inc.
# Copyright (c) 2023, Rob Norris <robn@despairlabs.com>
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/block_cloning/block_cloning.kshlib

verify_runnable "global"

claim="copy_file_range will fall back to copy when cloning on same txg"

log_assert $claim

typeset timeout=$(get_tunable TXG_TIMEOUT)

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 TXG_TIMEOUT $timeout
	log_must restore_tunable BCLONE_WAIT_DIRTY
}

log_onexit cleanup

log_must save_tunable BCLONE_WAIT_DIRTY

log_must set_tunable64 TXG_TIMEOUT 5000

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must sync_pool $TESTPOOL true

# Verify fallback to copy when there are dirty blocks
log_must set_tunable32 BCLONE_WAIT_DIRTY 0

log_must dd if=/dev/urandom of=/$TESTPOOL/file bs=128K count=4
log_must clonefile -f /$TESTPOOL/file /$TESTPOOL/clone 0 0 524288

log_must sync_pool $TESTPOOL

log_must have_same_content /$TESTPOOL/file /$TESTPOOL/clone

typeset blocks=$(get_same_blocks $TESTPOOL file $TESTPOOL clone)
log_must [ "$blocks" = "" ]

log_must rm /$TESTPOOL/file /$TESTPOOL/clone

# Verify blocks are cloned even when there are dirty blocks
log_must set_tunable32 BCLONE_WAIT_DIRTY 1

log_must dd if=/dev/urandom of=/$TESTPOOL/file bs=128K count=4
log_must clonefile -f /$TESTPOOL/file /$TESTPOOL/clone 0 0 524288

log_must sync_pool $TESTPOOL

log_must have_same_content /$TESTPOOL/file /$TESTPOOL/clone

typeset blocks=$(get_same_blocks $TESTPOOL file $TESTPOOL clone)
log_must [ "$blocks" = "0 1 2 3" ]

log_pass $claim

