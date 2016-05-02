/* Copyright 2016 Pedro Falcato

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef _SPINLOCK_H
#define _SPINLOCK_H

typedef struct spinlock
{
	unsigned long lock;
}spinlock_t;

void acquire(spinlock_t*);
void release(spinlock_t*);
void wait(spinlock_t*);
#endif
