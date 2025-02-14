/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "gtest/gtest.h"

#include "LogSuccessStorageProvider.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/VM/AlignedStorage.h"
#include "hermes/VM/LimitedStorageProvider.h"
#include "hermes/VM/LogFailStorageProvider.h"

#include "llvm/ADT/STLExtras.h"

using namespace hermes;
using namespace hermes::vm;

namespace {

/// Implementation of StorageProvider that always fails to allocate.
struct NullStorageProvider : public StorageProvider {
  static std::unique_ptr<NullStorageProvider> create();

  llvm::ErrorOr<void *> newStorage(const char *) override;
  void deleteStorage(void *) override;
};

/* static */
std::unique_ptr<NullStorageProvider> NullStorageProvider::create() {
  return llvm::make_unique<NullStorageProvider>();
}

llvm::ErrorOr<void *> NullStorageProvider::newStorage(const char *) {
  // Doesn't matter what code is returned here.
  return make_error_code(OOMError::TestVMLimitReached);
}

void NullStorageProvider::deleteStorage(void *) {}

TEST(StorageProviderTest, LogSuccessStorageProviderSuccess) {
  LogSuccessStorageProvider provider{StorageProvider::mmapProvider()};

  ASSERT_EQ(0, provider.numAllocated());
  ASSERT_EQ(0, provider.numDeleted());
  ASSERT_EQ(0, provider.numLive());

  auto result = provider.newStorage("Test");
  ASSERT_TRUE(result);
  void *s = result.get();

  EXPECT_EQ(1, provider.numAllocated());
  EXPECT_EQ(0, provider.numDeleted());
  EXPECT_EQ(1, provider.numLive());

  provider.deleteStorage(s);

  EXPECT_EQ(1, provider.numAllocated());
  EXPECT_EQ(1, provider.numDeleted());
  EXPECT_EQ(0, provider.numLive());
}

TEST(StorageProviderTest, LogSuccessStorageProviderFail) {
  LogSuccessStorageProvider provider{NullStorageProvider::create()};

  ASSERT_EQ(0, provider.numAllocated());
  ASSERT_EQ(0, provider.numDeleted());
  ASSERT_EQ(0, provider.numLive());

  auto result = provider.newStorage("Test");
  ASSERT_FALSE(result);

  EXPECT_EQ(0, provider.numAllocated());
  EXPECT_EQ(0, provider.numDeleted());
  EXPECT_EQ(0, provider.numLive());

  EXPECT_EQ(0, provider.numAllocated());
  EXPECT_EQ(0, provider.numDeleted());
  EXPECT_EQ(0, provider.numLive());
}

TEST(StorageProviderTest, LimitedStorageProviderEnforce) {
  constexpr size_t LIM = 2;
  LimitedStorageProvider provider{
      StorageProvider::mmapProvider(),
      AlignedStorage::size() * LIM,
  };
  void *live[LIM];
  for (size_t i = 0; i < LIM; ++i) {
    auto result = provider.newStorage("Live");
    ASSERT_TRUE(result);
    live[i] = result.get();
  }

  EXPECT_FALSE(provider.newStorage("Dead"));

  // Clean-up
  for (auto s : live) {
    provider.deleteStorage(s);
  }
}

TEST(StorageProviderTest, LimitedStorageProviderTrackDelete) {
  constexpr size_t LIM = 2;
  LimitedStorageProvider provider{
      StorageProvider::mmapProvider(),
      AlignedStorage::size() * LIM,
  };

  // If the storage gets deleted, we should be able to re-allocate it, even if
  // the total number of allocations exceeds the limit.
  for (size_t i = 0; i < LIM + 1; ++i) {
    auto result = provider.newStorage("Live");
    ASSERT_TRUE(result);
    auto *s = result.get();
    provider.deleteStorage(s);
  }
}

TEST(StorageProviderTest, LimitedStorageProviderDeleteNull) {
  constexpr size_t LIM = 2;
  LimitedStorageProvider provider{
      StorageProvider::mmapProvider(),
      AlignedStorage::size() * LIM,
  };

  void *live[LIM];

  for (size_t i = 0; i < LIM; ++i) {
    auto result = provider.newStorage("Live");
    ASSERT_TRUE(result);
    live[i] = result.get();
  }

  // The allocations should fail because we have hit the limit, and the
  // deletions should not affect the limit, because they are of null storages.
  for (size_t i = 0; i < 2; ++i) {
    auto result = provider.newStorage("Live");
    EXPECT_FALSE(result);
  }

  // Clean-up
  for (auto s : live) {
    provider.deleteStorage(s);
  }
}

TEST(StorageProviderTest, LogFailStorageProvider) {
  constexpr size_t LIM = 2;
  LimitedStorageProvider delegate{StorageProvider::mmapProvider(),
                                  AlignedStorage::size() * LIM};

  LogFailStorageProvider provider{&delegate};

  constexpr size_t FAILS = 3;
  void *storages[LIM];
  for (size_t i = 0; i < LIM; ++i) {
    auto result = provider.newStorage();
    ASSERT_TRUE(result);
    storages[i] = result.get();
  }
  for (size_t i = 0; i < FAILS; ++i) {
    auto result = provider.newStorage();
    ASSERT_FALSE(result);
  }

  EXPECT_EQ(FAILS, provider.numFailedAllocs());

  // Clean-up
  for (auto s : storages) {
    provider.deleteStorage(s);
  }
}

/// StorageGuard will free storage on scope exit.
class StorageGuard final {
 public:
  StorageGuard(std::shared_ptr<StorageProvider> provider, void *storage)
      : provider_(std::move(provider)), storage_(storage) {}

  ~StorageGuard() {
    provider_->deleteStorage(storage_);
  }

  void *raw() const {
    return storage_;
  }

 private:
  std::shared_ptr<StorageProvider> provider_;
  void *storage_;
};

TEST(StorageProviderTest, WithExcess) {
  auto result = StorageProvider::preAllocatedProvider(0, 0, 100);
  ASSERT_TRUE(result);
  std::shared_ptr<StorageProvider> provider{std::move(result.get())};
  // This should succeed even though the maxAmount is 0.
  // The excess bytes requested should be rounded up to give an extra storage
  // allocation.
  StorageGuard storage{provider, provider->newStorage().get()};
  EXPECT_TRUE(storage.raw());
  // A request for a second storage *can* fail, but is not required to.
}

#ifndef NDEBUG

class SetVALimit final {
 public:
  explicit SetVALimit(size_t vaLimit) {
    oscompat::set_test_vm_allocate_limit(vaLimit);
  }

  ~SetVALimit() {
    oscompat::unset_test_vm_allocate_limit();
  }
};

static const size_t KB = 1 << 10;
static const size_t MB = KB * KB;

TEST(StorageProviderTest, SucceedsWithoutReducing) {
  // Should succeed without reducing the size at all.
  SetVALimit limit{16 * MB};
  auto result = vmAllocateAllowLess(8 * MB, 1 * MB, 1 * MB);
  ASSERT_TRUE(result);
  auto memAndSize = result.get();
  EXPECT_NE(memAndSize.first, nullptr);
  EXPECT_EQ(memAndSize.second, 8 * MB);
}

TEST(StorageProviderTest, SucceedsAfterReducing) {
  {
    // Should succeed after reducing the size to below the limit.
    SetVALimit limit{40 * MB};
    auto result = vmAllocateAllowLess(100 * MB, 25 * MB, 1 * MB);
    ASSERT_TRUE(result);
    auto memAndSize = result.get();
    EXPECT_NE(memAndSize.first, nullptr);
    EXPECT_GE(memAndSize.second, 25 * MB);
    EXPECT_LE(memAndSize.second, 40 * MB);
  }
  {
    // Test using the AlignedStorage alignment
    SetVALimit limit{50 * AlignedStorage::size()};
    auto result = vmAllocateAllowLess(
        100 * AlignedStorage::size(),
        30 * AlignedStorage::size(),
        AlignedStorage::size());
    ASSERT_TRUE(result);
    auto memAndSize = result.get();
    EXPECT_NE(memAndSize.first, nullptr);
    EXPECT_GE(memAndSize.second, 30 * AlignedStorage::size());
    EXPECT_LE(memAndSize.second, 50 * AlignedStorage::size());
  }
}

TEST(StorageProviderTest, FailsDueToLimitLowerThanMin) {
  // Should not succeed, limit below min amount.
  SetVALimit limit{5 * MB};
  auto result = vmAllocateAllowLess(100 * MB, 10 * MB, 1 * MB);
  ASSERT_FALSE(result);
}

#endif

} // namespace
