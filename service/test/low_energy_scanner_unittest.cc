//
//  Copyright (C) 2016 The Android Open Source Project
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at:
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include <base/macros.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "service/adapter.h"
#include "service/hal/fake_bluetooth_gatt_interface.h"
#include "service/low_energy_scanner.h"
#include "stack/include/bt_types.h"
#include "stack/include/hcidefs.h"
#include "test/mock_adapter.h"

using ::testing::_;
using ::testing::Return;
using ::testing::Pointee;
using ::testing::DoAll;
using ::testing::Invoke;

namespace bluetooth {
namespace {

class MockGattHandler
    : public hal::FakeBluetoothGattInterface::TestScannerHandler {
 public:
  MockGattHandler() {
    ON_CALL(*this, Scan(false)).WillByDefault(Return(BT_STATUS_SUCCESS));
  }
  ~MockGattHandler() override = default;

  MOCK_METHOD1(RegisterScanner, bt_status_t(bt_uuid_t*));
  MOCK_METHOD1(UnregisterScanner, bt_status_t(int));
  MOCK_METHOD1(Scan, bt_status_t(bool));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockGattHandler);
};

class TestDelegate : public LowEnergyScanner::Delegate {
 public:
  TestDelegate() : scan_result_count_(0) {}

  ~TestDelegate() override = default;

  int scan_result_count() const { return scan_result_count_; }
  const ScanResult& last_scan_result() const { return last_scan_result_; }

  void OnScanResult(LowEnergyScanner* scanner, const ScanResult& scan_result) {
    ASSERT_TRUE(scanner);
    scan_result_count_++;
    last_scan_result_ = scan_result;
  }

 private:
  int scan_result_count_;
  ScanResult last_scan_result_;

  DISALLOW_COPY_AND_ASSIGN(TestDelegate);
};

class LowEnergyScannerTest : public ::testing::Test {
 public:
  LowEnergyScannerTest() = default;
  ~LowEnergyScannerTest() override = default;

  void SetUp() override {
    // Only set |mock_handler_| if a test hasn't set it.
    if (!mock_handler_) mock_handler_.reset(new MockGattHandler());
    fake_hal_gatt_iface_ = new hal::FakeBluetoothGattInterface(
        nullptr,
        std::static_pointer_cast<
            hal::FakeBluetoothGattInterface::TestScannerHandler>(mock_handler_),
        nullptr, nullptr);
    hal::BluetoothGattInterface::InitializeForTesting(fake_hal_gatt_iface_);
    ble_factory_.reset(new LowEnergyScannerFactory(mock_adapter_));
  }

  void TearDown() override {
    ble_factory_.reset();
    hal::BluetoothGattInterface::CleanUp();
  }

 protected:
  hal::FakeBluetoothGattInterface* fake_hal_gatt_iface_;
  testing::MockAdapter mock_adapter_;
  std::shared_ptr<MockGattHandler> mock_handler_;
  std::unique_ptr<LowEnergyScannerFactory> ble_factory_;

 private:
  DISALLOW_COPY_AND_ASSIGN(LowEnergyScannerTest);
};

// Used for tests that operate on a pre-registered scanner.
class LowEnergyScannerPostRegisterTest : public LowEnergyScannerTest {
 public:
  LowEnergyScannerPostRegisterTest() : next_scanner_id_(0) {}
  ~LowEnergyScannerPostRegisterTest() override = default;

  void SetUp() override {
    LowEnergyScannerTest::SetUp();
    auto callback = [&](std::unique_ptr<LowEnergyScanner> scanner) {
      le_scanner_ = std::move(scanner);
    };
    RegisterTestScanner(callback);
  }

  void TearDown() override {
    EXPECT_CALL(*mock_handler_, UnregisterScanner(_))
        .Times(1)
        .WillOnce(Return(BT_STATUS_SUCCESS));
    le_scanner_.reset();
    LowEnergyScannerTest::TearDown();
  }

  void RegisterTestScanner(
      const std::function<void(std::unique_ptr<LowEnergyScanner> scanner)>
          callback) {
    UUID uuid = UUID::GetRandom();
    auto api_callback = [&](BLEStatus status, const UUID& in_uuid,
                            std::unique_ptr<BluetoothInstance> in_scanner) {
      CHECK(in_uuid == uuid);
      CHECK(in_scanner.get());
      CHECK(status == BLE_STATUS_SUCCESS);

      callback(std::unique_ptr<LowEnergyScanner>(
          static_cast<LowEnergyScanner*>(in_scanner.release())));
    };

    EXPECT_CALL(*mock_handler_, RegisterScanner(_))
        .Times(1)
        .WillOnce(Return(BT_STATUS_SUCCESS));

    ble_factory_->RegisterInstance(uuid, api_callback);

    bt_uuid_t hal_uuid = uuid.GetBlueDroid();
    fake_hal_gatt_iface_->NotifyRegisterScannerCallback(0, next_scanner_id_++,
                                                        hal_uuid);
    ::testing::Mock::VerifyAndClearExpectations(mock_handler_.get());
  }

 protected:
  std::unique_ptr<LowEnergyScanner> le_scanner_;

 private:
  int next_scanner_id_;

  DISALLOW_COPY_AND_ASSIGN(LowEnergyScannerPostRegisterTest);
};

TEST_F(LowEnergyScannerTest, RegisterInstance) {
  EXPECT_CALL(*mock_handler_, RegisterScanner(_))
      .Times(2)
      .WillOnce(Return(BT_STATUS_FAIL))
      .WillOnce(Return(BT_STATUS_SUCCESS));

  // These will be asynchronously populated with a result when the callback
  // executes.
  BLEStatus status = BLE_STATUS_SUCCESS;
  UUID cb_uuid;
  std::unique_ptr<LowEnergyScanner> scanner;
  int callback_count = 0;

  auto callback = [&](BLEStatus in_status, const UUID& uuid,
                      std::unique_ptr<BluetoothInstance> in_scanner) {
    status = in_status;
    cb_uuid = uuid;
    scanner = std::unique_ptr<LowEnergyScanner>(
        static_cast<LowEnergyScanner*>(in_scanner.release()));
    callback_count++;
  };

  UUID uuid0 = UUID::GetRandom();

  // HAL returns failure.
  EXPECT_FALSE(ble_factory_->RegisterInstance(uuid0, callback));
  EXPECT_EQ(0, callback_count);

  // HAL returns success.
  EXPECT_TRUE(ble_factory_->RegisterInstance(uuid0, callback));
  EXPECT_EQ(0, callback_count);

  // Calling twice with the same UUID should fail with no additional call into
  // the stack.
  EXPECT_FALSE(ble_factory_->RegisterInstance(uuid0, callback));

  ::testing::Mock::VerifyAndClearExpectations(mock_handler_.get());

  // Call with a different UUID while one is pending.
  UUID uuid1 = UUID::GetRandom();
  EXPECT_CALL(*mock_handler_, RegisterScanner(_))
      .Times(1)
      .WillOnce(Return(BT_STATUS_SUCCESS));
  EXPECT_TRUE(ble_factory_->RegisterInstance(uuid1, callback));

  // Trigger callback with an unknown UUID. This should get ignored.
  UUID uuid2 = UUID::GetRandom();
  bt_uuid_t hal_uuid = uuid2.GetBlueDroid();
  fake_hal_gatt_iface_->NotifyRegisterScannerCallback(0, 0, hal_uuid);
  EXPECT_EQ(0, callback_count);

  // |uuid0| succeeds.
  int scanner_if0 = 2;  // Pick something that's not 0.
  hal_uuid = uuid0.GetBlueDroid();
  fake_hal_gatt_iface_->NotifyRegisterScannerCallback(BT_STATUS_SUCCESS,
                                                      scanner_if0, hal_uuid);

  EXPECT_EQ(1, callback_count);
  ASSERT_TRUE(scanner.get() !=
              nullptr);  // Assert to terminate in case of error
  EXPECT_EQ(BLE_STATUS_SUCCESS, status);
  EXPECT_EQ(scanner_if0, scanner->GetInstanceId());
  EXPECT_EQ(uuid0, scanner->GetAppIdentifier());
  EXPECT_EQ(uuid0, cb_uuid);

  // The scanner should unregister itself when deleted.
  EXPECT_CALL(*mock_handler_, UnregisterScanner(scanner_if0))
      .Times(1)
      .WillOnce(Return(BT_STATUS_SUCCESS));
  scanner.reset();
  ::testing::Mock::VerifyAndClearExpectations(mock_handler_.get());

  // |uuid1| fails.
  int scanner_if1 = 3;
  hal_uuid = uuid1.GetBlueDroid();
  fake_hal_gatt_iface_->NotifyRegisterScannerCallback(BT_STATUS_FAIL,
                                                      scanner_if1, hal_uuid);

  EXPECT_EQ(2, callback_count);
  ASSERT_TRUE(scanner.get() ==
              nullptr);  // Assert to terminate in case of error
  EXPECT_EQ(BLE_STATUS_FAILURE, status);
  EXPECT_EQ(uuid1, cb_uuid);
}

TEST_F(LowEnergyScannerPostRegisterTest, ScanSettings) {
  EXPECT_CALL(mock_adapter_, IsEnabled())
      .WillOnce(Return(false))
      .WillRepeatedly(Return(true));

  ScanSettings settings;
  std::vector<ScanFilter> filters;

  // Adapter is not enabled.
  EXPECT_FALSE(le_scanner_->StartScan(settings, filters));

  // TODO(jpawlowski): add tests checking settings and filter parsing when
  // implemented

  // These should succeed and result in a HAL call
  EXPECT_CALL(*mock_handler_, Scan(true))
      .Times(1)
      .WillOnce(Return(BT_STATUS_SUCCESS));
  EXPECT_TRUE(le_scanner_->StartScan(settings, filters));

  // These should succeed and result in a HAL call
  EXPECT_CALL(*mock_handler_, Scan(false))
      .Times(1)
      .WillOnce(Return(BT_STATUS_SUCCESS));
  EXPECT_TRUE(le_scanner_->StopScan());

  ::testing::Mock::VerifyAndClearExpectations(mock_handler_.get());
}

TEST_F(LowEnergyScannerPostRegisterTest, ScanRecord) {
  TestDelegate delegate;
  le_scanner_->SetDelegate(&delegate);

  EXPECT_EQ(0, delegate.scan_result_count());

  vector<uint8_t> kTestRecord0({0x02, 0x01, 0x00, 0x00});
  vector<uint8_t> kTestRecord1({0x00});
  vector<uint8_t> kTestRecord2(
      {0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
       0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
       0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
       0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00});
  const bt_bdaddr_t kTestAddress = {{0x01, 0x02, 0x03, 0x0A, 0x0B, 0x0C}};
  const char kTestAddressStr[] = "01:02:03:0A:0B:0C";
  const int kTestRssi = 64;

  // Scan wasn't started. Result should be ignored.
  fake_hal_gatt_iface_->NotifyScanResultCallback(kTestAddress, kTestRssi,
                                                 kTestRecord0);
  EXPECT_EQ(0, delegate.scan_result_count());

  // Start a scan session for |le_scanner_|.
  EXPECT_CALL(mock_adapter_, IsEnabled()).Times(1).WillOnce(Return(true));
  EXPECT_CALL(*mock_handler_, Scan(_))
      .Times(2)
      .WillOnce(Return(BT_STATUS_SUCCESS))
      .WillOnce(Return(BT_STATUS_SUCCESS));
  ScanSettings settings;
  std::vector<ScanFilter> filters;
  ASSERT_TRUE(le_scanner_->StartScan(settings, filters));

  fake_hal_gatt_iface_->NotifyScanResultCallback(kTestAddress, kTestRssi,
                                                 kTestRecord0);
  EXPECT_EQ(1, delegate.scan_result_count());
  EXPECT_EQ(kTestAddressStr, delegate.last_scan_result().device_address());
  EXPECT_EQ(kTestRssi, delegate.last_scan_result().rssi());
  EXPECT_EQ(3U, delegate.last_scan_result().scan_record().size());

  fake_hal_gatt_iface_->NotifyScanResultCallback(kTestAddress, kTestRssi,
                                                 kTestRecord1);
  EXPECT_EQ(2, delegate.scan_result_count());
  EXPECT_EQ(kTestAddressStr, delegate.last_scan_result().device_address());
  EXPECT_EQ(kTestRssi, delegate.last_scan_result().rssi());
  EXPECT_TRUE(delegate.last_scan_result().scan_record().empty());

  fake_hal_gatt_iface_->NotifyScanResultCallback(kTestAddress, kTestRssi,
                                                 kTestRecord2);
  EXPECT_EQ(3, delegate.scan_result_count());
  EXPECT_EQ(kTestAddressStr, delegate.last_scan_result().device_address());
  EXPECT_EQ(kTestRssi, delegate.last_scan_result().rssi());
  EXPECT_EQ(62U, delegate.last_scan_result().scan_record().size());

  le_scanner_->SetDelegate(nullptr);
}

}  // namespace
}  // namespace bluetooth