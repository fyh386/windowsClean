#include "recycle.h"

#include "util.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <atomic>

namespace {

constexpr size_t kChunkSize = 5000;

bool IsRemotePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') {
        wchar_t root[4] = { path[0], L':', L'\\', 0 };
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

class DeleteSink : public IFileOperationProgressSink {
public:
    DeleteSink(DeleteStats& stats, const std::vector<std::wstring>* paths,
               size_t base, size_t total, DeleteProgress progress)
        : stats_(stats), paths_(paths), base_(base), total_(total), progress_(progress) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* psiItem, HRESULT hrDelete, IShellItem*) override {
        size_t idx = base_ + stats_.ok + stats_.failed;
        if (SUCCEEDED(hrDelete)) {
            ++stats_.ok;
        } else {
            ++stats_.failed;
            if (paths_ && idx < paths_->size()) stats_.failedPaths.push_back((*paths_)[idx]);
        }
        if (progress_) progress_(base_ + stats_.ok + stats_.failed, total_);
        return S_OK;
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{ 1 };
    DeleteStats& stats_;
    const std::vector<std::wstring>* paths_;
    size_t base_;
    size_t total_;
    DeleteProgress progress_;
};

DeleteStats DoBatch(const std::vector<std::wstring>& paths, bool toRecycleBin,
                    size_t base, size_t total, DeleteProgress progress) {
    DeleteStats stats;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(hr);

    for (size_t off = 0; off < paths.size(); off += kChunkSize) {
        size_t n = std::min(kChunkSize, paths.size() - off);

        IFileOperation* opRaw = nullptr;
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&opRaw));
        if (FAILED(hr) || !opRaw) {
            stats.failed += n;
            for (size_t i = off; i < off + n; ++i) stats.failedPaths.push_back(paths[i]);
            if (progress) progress(base + stats.ok + stats.failed, total);
            continue;
        }

        DWORD flags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        if (toRecycleBin) flags |= FOF_ALLOWUNDO;
        opRaw->SetOperationFlags(flags);

        bool anyAdded = false;
        for (size_t i = off; i < off + n; ++i) {
            IShellItem* item = nullptr;
            std::wstring longPath;
            if (paths[i].size() > 247) longPath = AddLongPathPrefix(paths[i]);
            const wchar_t* p = longPath.empty() ? paths[i].c_str() : longPath.c_str();
            if (SUCCEEDED(SHCreateItemFromParsingName(p, nullptr, IID_PPV_ARGS(&item)))) {
                if (SUCCEEDED(opRaw->DeleteItem(item, nullptr))) anyAdded = true;
                item->Release();
            } else {
                ++stats.failed;
                stats.failedPaths.push_back(paths[i]);
            }
        }

        if (anyAdded) {
            DeleteSink* sink = new DeleteSink(stats, &paths, base, total, progress);
            DWORD cookie = 0;
            opRaw->Advise(sink, &cookie);
            opRaw->PerformOperations();
            opRaw->Unadvise(cookie);
            sink->Release();
        }
        opRaw->Release();
    }

    if (needUninit) CoUninitialize();
    return stats;
}

} // namespace

DeleteStats DeletePathsToRecycleBin(const std::vector<std::wstring>& paths, DeleteProgress progress) {
    DeleteStats stats;
    if (paths.empty()) return stats;

    std::vector<std::wstring> local, remote;
    for (const auto& p : paths) {
        if (IsRemotePath(p)) remote.push_back(p);
        else local.push_back(p);
    }

    if (!local.empty()) {
        DeleteStats s = DoBatch(local, true, 0, paths.size(), progress);
        stats.ok += s.ok;
        stats.failed += s.failed;
        stats.failedPaths.insert(stats.failedPaths.end(), s.failedPaths.begin(), s.failedPaths.end());
    }
    if (!remote.empty()) {
        size_t base = local.size();
        stats.permanent = remote.size();
        DeleteStats s = DoBatch(remote, false, base, paths.size(), progress);
        stats.ok += s.ok;
        stats.failed += s.failed;
        stats.failedPaths.insert(stats.failedPaths.end(), s.failedPaths.begin(), s.failedPaths.end());
    }
    return stats;
}

bool WritePathListFile(const std::wstring& file, const std::vector<std::wstring>& paths) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    DWORD count = (DWORD)paths.size();
    DWORD written = 0;
    if (!WriteFile(h, &count, sizeof(count), &written, nullptr) || written != sizeof(count)) ok = false;
    for (const auto& p : paths) {
        DWORD len = (DWORD)p.size();
        written = 0;
        if (!WriteFile(h, &len, sizeof(len), &written, nullptr) || written != sizeof(len)) { ok = false; break; }
        written = 0;
        if (!WriteFile(h, p.data(), len * sizeof(wchar_t), &written, nullptr) ||
            written != len * sizeof(wchar_t)) { ok = false; break; }
    }
    CloseHandle(h);
    return ok;
}

bool ReadPathListFile(const std::wstring& file, std::vector<std::wstring>& out) {
    std::vector<unsigned char> raw;
    if (!ReadBinaryFile(file, raw)) return false;
    size_t pos = 0;
    if (raw.size() < sizeof(DWORD)) return false;
    DWORD count = 0;
    memcpy(&count, raw.data(), sizeof(count));
    pos = sizeof(count);
    out.clear();
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (raw.size() - pos < sizeof(DWORD)) return false;
        DWORD len = 0;
        memcpy(&len, raw.data() + pos, sizeof(len));
        pos += sizeof(len);
        size_t bytes = (size_t)len * sizeof(wchar_t);
        if (raw.size() - pos < bytes) return false;
        out.emplace_back((const wchar_t*)(raw.data() + pos), len);
        pos += bytes;
    }
    return true;
}
