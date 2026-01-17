/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

#include "../src/core_engine/LockFreeQueue.h"
#include <cassert>
#include <iostream>
#include <vector>

void test_skipped_slot() {
  std::cout << "Testing MPSCQueue skipped slot handling..." << std::endl;

  MPSCQueue<int, 16> queue;
  int a = 10;
  int b = 20;
  int c = 30;

  // 1. Enqueue valid item A
  queue.enqueue(&a);

  // 2. Enqueue nullptr (simulating a skipped/dropped slot due to producer
  // timeout) The producer logic in LockFreeQueue.h explicitly sets
  // data=nullptr, ready=true on timeout. We simulate this by directly enqueuing
  // nullptr.
  queue.enqueue(nullptr);

  // 3. Enqueue valid item B
  queue.enqueue(&b);

  // 4. Enqueue valid item C
  queue.enqueue(&c);

  // 5. Dequeue A
  int *res1 = queue.dequeue();
  assert(res1 != nullptr);
  assert(*res1 == 10);
  std::cout << "  Passed: Dequeued A" << std::endl;

  // 6. Dequeue B (Should skip the nullptr slot automatically)
  int *res2 = queue.dequeue();
  // In the BUGGY version, this would return nullptr (interpreted as empty).
  // In the FIXED version, this should loop past the nullptr and return B.
  assert(res2 != nullptr);
  assert(*res2 == 20);
  std::cout << "  Passed: Dequeued B (skipped null slot)" << std::endl;

  // 7. Dequeue C
  int *res3 = queue.dequeue();
  assert(res3 != nullptr);
  assert(*res3 == 30);
  std::cout << "  Passed: Dequeued C" << std::endl;

  // 8. Dequeue should be empty now
  int *res4 = queue.dequeue();
  assert(res4 == nullptr);
  std::cout << "  Passed: Queue empty" << std::endl;

  std::cout << "MPSCQueue test passed!" << std::endl;
}

int main() {
  test_skipped_slot();
  return 0;
}
