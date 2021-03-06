/*
 * fcntl_implem.h
 * Stub implementation for fcntl call that intended to use by any zrt mount type.
 * Only single process can operate with a lock, as not supported multiprocess environment.
 * It is used interface to get access to mount specific implementations;
 *
 * Copyright (c) 2012-2013, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef __FCNTL_IMPLEM_H__
#define __FCNTL_IMPLEM_H__

#include <unistd.h>
#include <fcntl.h>

#include "mount_specific_interface.h" //struct MountSpecificPublicInterface

int fcntl_implem(struct MountSpecificPublicInterface* this_, 
		 int fd, int cmd, ...);

#endif //__FCNTL_IMPLEM_H__

