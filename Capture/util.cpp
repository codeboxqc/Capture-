#include <windows.h>
#include <iostream>

// Use large pages to lock RAM buffering in memory for zero-drop NVMe writing
void EnableLargePages() {
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }
        CloseHandle(hToken);
    }
}



 


/*

 

// Set multimedia priority
void SetMultimediaPriority(HANDLE thread) {
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    if (task) {
        AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
    }
}

// Use GPU timing queries for latency monitoring
ID3D12QueryHeap* m_timestampHeap;
ID3D12Resource* m_timestampReadbackBuffer;

void SetupGPUTiming() {
    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Count = 256;
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    m_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_timestampHeap));
}

// Query optimal NVMe settings
void OptimizeNVMe() {
    // Get disk performance characteristics
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek;
    DWORD bytesReturned;
    
    DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        &seek, sizeof(seek),
        &bytesReturned, nullptr);
}

*/