#include "core/accessibility/OverlayUiaProvider.h"

// WIN32_LEAN_AND_MEAN removes COM declarations from windows.h in this project.
// UIAutomation headers require them before their MIDL forward declarations.
#include <ole2.h>
#include <UIAutomation.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

namespace tools3000::core::accessibility {
namespace {

std::mutex g_providerMutex;
std::unordered_map<HWND, IRawElementProviderSimple*> g_liveProviders;

HRESULT setBoolean(VARIANT* result, bool value) {
    result->vt = VT_BOOL;
    result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT setBoundingRectangle(VARIANT* result, const RECT& rect) {
    SAFEARRAY* values = SafeArrayCreateVector(VT_R8, 0, 4);
    if (!values) return E_OUTOFMEMORY;
    const double rectangle[] = {static_cast<double>(rect.left), static_cast<double>(rect.top),
                                static_cast<double>(rect.right - rect.left),
                                static_cast<double>(rect.bottom - rect.top)};
    for (LONG index = 0; index < 4; ++index) {
        if (FAILED(SafeArrayPutElement(values, &index, const_cast<double*>(&rectangle[index])))) {
            SafeArrayDestroy(values);
            return E_OUTOFMEMORY;
        }
    }
    result->vt = VT_ARRAY | VT_R8;
    result->parray = values;
    return S_OK;
}

HRESULT setBoundingRectangle(VARIANT* result, HWND hwnd) {
    RECT rect{};
    if (!hwnd || !IsWindow(hwnd) || !GetWindowRect(hwnd, &rect)) return S_OK;
    return setBoundingRectangle(result, rect);
}

void retainProvider(HWND hwnd, IRawElementProviderSimple* provider) noexcept {
    IRawElementProviderSimple* old = nullptr;
    {
        std::lock_guard lock(g_providerMutex);
        if (const auto it = g_liveProviders.find(hwnd); it != g_liveProviders.end()) {
            old = it->second;
            it->second = provider;
        } else {
            g_liveProviders.emplace(hwnd, provider);
        }
        provider->AddRef();
    }
    if (old) {
        UiaDisconnectProvider(old);
        old->Release();
    }
}

void disconnectProvider(HWND hwnd) noexcept {
    IRawElementProviderSimple* provider = nullptr;
    {
        std::lock_guard lock(g_providerMutex);
        if (const auto it = g_liveProviders.find(hwnd); it != g_liveProviders.end()) {
            provider = it->second;
            g_liveProviders.erase(it);
        }
    }
    if (provider) {
        UiaDisconnectProvider(provider);
        provider->Release();
    }
}

class OverlayUiaProvider final : public IRawElementProviderSimple {
public:
    OverlayUiaProvider(HWND hwnd, const OverlayUiaSemantics& semantics)
        : m_hwnd(hwnd)
        , m_automationId(semantics.automationId)
        , m_helpText(semantics.helpText)
        , m_role(semantics.role)
        , m_politeLiveRegion(semantics.politeLiveRegion) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) {
            *result = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_references;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --m_references;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
        if (!result) return E_POINTER;
        *result = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        // These overlays intentionally do not accept focus or pointer input;
        // exposing an Invoke pattern would give assistive tech a false action.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result);
        switch (propertyId) {
            case UIA_NamePropertyId:
                return setBstr(result, windowName());
            case UIA_AutomationIdPropertyId:
                return setBstr(result, m_automationId);
            case UIA_HelpTextPropertyId:
                return setBstr(result, m_helpText);
            case UIA_ControlTypePropertyId:
                result->vt = VT_I4;
                result->lVal = controlType();
                return S_OK;
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId:
            case UIA_IsEnabledPropertyId:
                return setBoolean(result, true);
            case UIA_IsOffscreenPropertyId:
                return setBoolean(result, !m_hwnd || !IsWindowVisible(m_hwnd));
            case UIA_BoundingRectanglePropertyId:
                return setBoundingRectangle(result, m_hwnd);
            case UIA_LiveSettingPropertyId:
                if (!m_politeLiveRegion) return S_OK;
                result->vt = VT_I4;
                result->lVal = static_cast<long>(Polite);
                return S_OK;
            default:
                return S_OK;
        }
    }

    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return UiaHostProviderFromHwnd(m_hwnd, result);
    }

private:
    static HRESULT setBstr(VARIANT* result, const std::wstring& value) {
        result->vt = VT_BSTR;
        result->bstrVal = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        return value.empty() || result->bstrVal ? S_OK : E_OUTOFMEMORY;
    }

    std::wstring windowName() const {
        if (!m_hwnd || !IsWindow(m_hwnd)) return m_helpText;
        const int length = GetWindowTextLengthW(m_hwnd);
        if (length <= 0) return m_helpText;
        // GetWindowTextW writes a terminating NUL in addition to its reported
        // character count, so reserve that slot instead of relying on string
        // capacity beyond size().
        std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(m_hwnd, name.data(), length + 1);
        name.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
        return name.empty() ? m_helpText : name;
    }

    long controlType() const noexcept {
        switch (m_role) {
            case OverlayUiaRole::Status: return UIA_StatusBarControlTypeId;
            case OverlayUiaRole::Pane: return UIA_PaneControlTypeId;
            case OverlayUiaRole::Text: return UIA_TextControlTypeId;
        }
        return UIA_TextControlTypeId;
    }

    std::atomic<ULONG> m_references{1};
    HWND m_hwnd = nullptr;
    std::wstring m_automationId;
    std::wstring m_helpText;
    OverlayUiaRole m_role = OverlayUiaRole::Text;
    bool m_politeLiveRegion = false;
};

class ActionRootProvider;

class ActionProvider final : public IRawElementProviderSimple,
                             public IRawElementProviderFragment,
                             public IInvokeProvider,
                             public ISelectionItemProvider {
public:
    ActionProvider(ActionRootProvider* root, std::size_t index);
    ~ActionProvider();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --m_references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override;
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override { return UIA_E_NOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override;
    HRESULT STDMETHODCALLTYPE Invoke() override;
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override;
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override;
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** result) override;
private:
    std::atomic<ULONG> m_references{1};
    ActionRootProvider* m_root = nullptr;
    std::size_t m_index = 0;
};

class ActionRootProvider final : public IRawElementProviderSimple,
                                 public IRawElementProviderFragment,
                                 public IRawElementProviderFragmentRoot,
                                 public ISelectionProvider {
public:
    ActionRootProvider(HWND hwnd, const OverlayUiaSemantics& semantics,
                       std::vector<OverlayUiaAction> actions)
        : m_hwnd(hwnd), m_automationId(semantics.automationId), m_helpText(semantics.helpText),
          m_actions(std::move(actions)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderFragmentRoot)) *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *result = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderSimple)) *result = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(ISelectionProvider)) *result = static_cast<ISelectionProvider*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --m_references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override { if (!result) return E_POINTER; *result = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (pattern == UIA_SelectionPatternId) {
            *result = static_cast<ISelectionProvider*>(this);
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override {
        if (!result) return E_POINTER; VariantInit(result);
        if (property == UIA_NamePropertyId) return setBstr(result, m_helpText);
        if (property == UIA_AutomationIdPropertyId) return setBstr(result, m_automationId);
        if (property == UIA_HelpTextPropertyId) return setBstr(result, m_helpText);
        if (property == UIA_ControlTypePropertyId) { result->vt=VT_I4; result->lVal=UIA_ToolBarControlTypeId; }
        else if (property == UIA_IsControlElementPropertyId || property == UIA_IsContentElementPropertyId || property == UIA_IsEnabledPropertyId) return setBoolean(result, true);
        else if (property == UIA_IsOffscreenPropertyId) return setBoolean(result, !m_hwnd || !IsWindowVisible(m_hwnd));
        else if (property == UIA_BoundingRectanglePropertyId) return setBoundingRectangle(result, m_hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override { if (!result) return E_POINTER; *result=nullptr; return UiaHostProviderFromHwnd(m_hwnd,result); }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override { return runtimeId(m_hwnd, 0, result); }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override { if (!result) return E_POINTER; RECT r{}; GetWindowRect(m_hwnd,&r); *result={double(r.left),double(r.top),double(r.right-r.left),double(r.bottom-r.top)}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override { if (!result) return E_POINTER; *result=nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return UIA_E_NOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override { if (!result) return E_POINTER; *result=this; AddRef(); return S_OK; }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,IRawElementProviderFragment** result) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override { if (!result) return E_POINTER; *result=nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** result) override;
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* result) override { if (!result) return E_POINTER; *result = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* result) override { if (!result) return E_POINTER; *result = FALSE; return S_OK; }
    const OverlayUiaAction* action(std::size_t index) const { return index < m_actions.size() ? &m_actions[index] : nullptr; }
    std::size_t count() const { return m_actions.size(); }
    HWND hwnd() const { return m_hwnd; }
    IRawElementProviderFragment* child(std::size_t index) { return index < m_actions.size() ? static_cast<IRawElementProviderFragment*>(new (std::nothrow) ActionProvider(this,index)) : nullptr; }
    static HRESULT setBstr(VARIANT* result,const std::wstring& value) { result->vt=VT_BSTR; result->bstrVal=SysAllocStringLen(value.data(),static_cast<UINT>(value.size())); return value.empty()||result->bstrVal?S_OK:E_OUTOFMEMORY; }
    static HRESULT runtimeId(HWND hwnd, long id, SAFEARRAY** result) {
        if (!result) return E_POINTER;
        *result = SafeArrayCreateVector(VT_I4, 0, 4);
        if (!*result) return E_OUTOFMEMORY;
        const auto handle = static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(hwnd));
        const long values[] = {UiaAppendRuntimeId,
                               static_cast<long>(handle & 0xFFFFFFFFu),
                               static_cast<long>((handle >> 32u) & 0xFFFFFFFFu), id};
        for (LONG index = 0; index < 4; ++index) {
            if (FAILED(SafeArrayPutElement(*result, &index, const_cast<long*>(&values[index])))) {
                SafeArrayDestroy(*result);
                *result = nullptr;
                return E_OUTOFMEMORY;
            }
        }
        return S_OK;
    }
private: std::atomic<ULONG> m_references{1}; HWND m_hwnd{}; std::wstring m_automationId,m_helpText; std::vector<OverlayUiaAction> m_actions;
};

ActionProvider::ActionProvider(ActionRootProvider* root,std::size_t index):m_root(root),m_index(index){ if(m_root)m_root->AddRef(); }
ActionProvider::~ActionProvider(){ if(m_root)m_root->Release(); }
HRESULT ActionProvider::QueryInterface(REFIID iid,void** result){ if(!result)return E_POINTER;*result=nullptr; if(iid==__uuidof(IUnknown)||iid==__uuidof(IRawElementProviderFragment))*result=static_cast<IRawElementProviderFragment*>(this); else if(iid==__uuidof(IRawElementProviderSimple))*result=static_cast<IRawElementProviderSimple*>(this); else if(iid==__uuidof(IInvokeProvider))*result=static_cast<IInvokeProvider*>(this); else if(iid==__uuidof(ISelectionItemProvider))*result=static_cast<ISelectionItemProvider*>(this); else return E_NOINTERFACE; AddRef();return S_OK; }
HRESULT ActionProvider::get_ProviderOptions(ProviderOptions* r){if(!r)return E_POINTER;*r=ProviderOptions_ServerSideProvider;return S_OK;}
HRESULT ActionProvider::GetPatternProvider(PATTERNID p,IUnknown** r){
    if(!r)return E_POINTER;
    *r=nullptr;
    const auto* action=m_root?m_root->action(m_index):nullptr;
    if(p==UIA_InvokePatternId && action && action->enabled && action->invokeMessage &&
       action->role==OverlayUiaActionRole::Button){*r=static_cast<IInvokeProvider*>(this);AddRef();}
    else if (p == UIA_SelectionItemPatternId && action && action->role == OverlayUiaActionRole::Button) {
        *r = static_cast<ISelectionItemProvider*>(this);
        AddRef();
    }
    return S_OK;
}
HRESULT ActionProvider::GetPropertyValue(PROPERTYID p, VARIANT* r) {
    if (!r) return E_POINTER;
    VariantInit(r);
    const auto* action = m_root ? m_root->action(m_index) : nullptr;
    if (!action || !m_root || !IsWindow(m_root->hwnd())) return UIA_E_ELEMENTNOTAVAILABLE;
    if (p == UIA_NamePropertyId) return ActionRootProvider::setBstr(r, action->name);
    if (p == UIA_AutomationIdPropertyId) return ActionRootProvider::setBstr(r, action->automationId);
    if (p == UIA_HelpTextPropertyId) return ActionRootProvider::setBstr(r, action->helpText);
    if (p == UIA_AcceleratorKeyPropertyId) return ActionRootProvider::setBstr(r, action->keyboardShortcut);
    if (p == UIA_ControlTypePropertyId) { r->vt = VT_I4; r->lVal = action->role == OverlayUiaActionRole::Button ? UIA_ButtonControlTypeId : UIA_TextControlTypeId; return S_OK; }
    if (p == UIA_IsControlElementPropertyId || p == UIA_IsContentElementPropertyId) return setBoolean(r, true);
    if (p == UIA_IsEnabledPropertyId) return setBoolean(r, action->enabled);
    if (p == UIA_IsKeyboardFocusablePropertyId) return setBoolean(r, false);
    if (p == UIA_IsOffscreenPropertyId) return setBoolean(r, !IsWindowVisible(m_root->hwnd()));
    if (p == UIA_BoundingRectanglePropertyId) return setBoundingRectangle(r, action->bounds);
    if (p == UIA_SelectionItemIsSelectedPropertyId) return setBoolean(r, action->selected);
    return S_OK;
}
HRESULT ActionProvider::get_HostRawElementProvider(IRawElementProviderSimple** r){if(!r)return E_POINTER;*r=nullptr;return S_OK;}
HRESULT ActionProvider::Navigate(NavigateDirection d,IRawElementProviderFragment** r){if(!r)return E_POINTER;*r=nullptr;if(!m_root)return UIA_E_ELEMENTNOTAVAILABLE;if(d==NavigateDirection_Parent){return m_root->QueryInterface(__uuidof(IRawElementProviderFragment),reinterpret_cast<void**>(r));}if(d==NavigateDirection_NextSibling&&m_index+1<m_root->count())*r=m_root->child(m_index+1);if(d==NavigateDirection_PreviousSibling&&m_index>0)*r=m_root->child(m_index-1);return S_OK;}
HRESULT ActionProvider::GetRuntimeId(SAFEARRAY**r){return ActionRootProvider::runtimeId(m_root ? m_root->hwnd() : nullptr,static_cast<long>(m_index+1),r);}
HRESULT ActionProvider::get_BoundingRectangle(UiaRect*r){if(!r)return E_POINTER;const auto*a=m_root?m_root->action(m_index):nullptr;if(!a)return UIA_E_ELEMENTNOTAVAILABLE;*r={double(a->bounds.left),double(a->bounds.top),double(a->bounds.right-a->bounds.left),double(a->bounds.bottom-a->bounds.top)};return S_OK;}
HRESULT ActionProvider::GetEmbeddedFragmentRoots(SAFEARRAY**r){if(!r)return E_POINTER;*r=nullptr;return S_OK;}
HRESULT ActionProvider::get_FragmentRoot(IRawElementProviderFragmentRoot**r){if(!r)return E_POINTER;*r=nullptr;return m_root?m_root->QueryInterface(__uuidof(IRawElementProviderFragmentRoot),reinterpret_cast<void**>(r)):UIA_E_ELEMENTNOTAVAILABLE;}
HRESULT ActionProvider::Invoke(){const auto*a=m_root?m_root->action(m_index):nullptr;if(!a||!a->enabled||!a->invokeMessage||!IsWindow(m_root->hwnd()))return UIA_E_ELEMENTNOTAVAILABLE;return PostMessageW(m_root->hwnd(),a->invokeMessage,a->invokeWParam,0)?S_OK:HRESULT_FROM_WIN32(GetLastError());}
HRESULT ActionProvider::Select() { return Invoke(); }
HRESULT ActionProvider::AddToSelection() { return Invoke(); }
HRESULT ActionProvider::RemoveFromSelection() { return UIA_E_NOTSUPPORTED; }
HRESULT ActionProvider::get_IsSelected(BOOL* result) {
    if (!result) return E_POINTER;
    const auto* action = m_root ? m_root->action(m_index) : nullptr;
    if (!action || !m_root || !IsWindow(m_root->hwnd())) return UIA_E_ELEMENTNOTAVAILABLE;
    *result = action->selected ? TRUE : FALSE;
    return S_OK;
}
HRESULT ActionProvider::get_SelectionContainer(IRawElementProviderSimple** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!m_root || !IsWindow(m_root->hwnd())) return UIA_E_ELEMENTNOTAVAILABLE;
    return m_root->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(result));
}
HRESULT ActionRootProvider::GetSelection(SAFEARRAY** result) {
    if (!result) return E_POINTER;
    const auto selectedCount = static_cast<ULONG>(std::count_if(
        m_actions.begin(), m_actions.end(), [](const OverlayUiaAction& action) { return action.selected; }));
    *result = SafeArrayCreateVector(VT_UNKNOWN, 0, selectedCount);
    if (!*result) return E_OUTOFMEMORY;
    LONG outputIndex = 0;
    for (std::size_t index = 0; index < m_actions.size(); ++index) {
        if (!m_actions[index].selected) continue;
        IRawElementProviderFragment* childProvider = child(index);
        if (!childProvider) { SafeArrayDestroy(*result); *result = nullptr; return E_OUTOFMEMORY; }
        IRawElementProviderSimple* simple = nullptr;
        const HRESULT queryResult = childProvider->QueryInterface(
            __uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&simple));
        childProvider->Release();
        if (FAILED(queryResult)) { SafeArrayDestroy(*result); *result = nullptr; return queryResult; }
        const HRESULT putResult = SafeArrayPutElement(*result, &outputIndex, simple);
        simple->Release();
        if (FAILED(putResult)) { SafeArrayDestroy(*result); *result = nullptr; return putResult; }
        ++outputIndex;
    }
    return S_OK;
}
HRESULT ActionRootProvider::Navigate(NavigateDirection d,IRawElementProviderFragment**r){if(!r)return E_POINTER;*r=nullptr;if(d==NavigateDirection_FirstChild&&!m_actions.empty())*r=child(0);else if(d==NavigateDirection_LastChild&&!m_actions.empty())*r=child(m_actions.size()-1);return S_OK;}
HRESULT ActionRootProvider::ElementProviderFromPoint(double x,double y,IRawElementProviderFragment**r){if(!r)return E_POINTER;*r=nullptr;for(std::size_t i=0;i<m_actions.size();++i){const auto&b=m_actions[i].bounds;if(x>=b.left&&x<=b.right&&y>=b.top&&y<=b.bottom){*r=child(i);return *r?S_OK:E_OUTOFMEMORY;}}return QueryInterface(__uuidof(IRawElementProviderFragment),reinterpret_cast<void**>(r));}

}  // namespace

LRESULT respondToOverlayUiaGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam,
                                     const OverlayUiaSemantics& semantics) noexcept {
    if (!hwnd || lParam != UiaRootObjectId) return 0;
    auto* provider = new (std::nothrow) OverlayUiaProvider(hwnd, semantics);
    if (!provider) return 0;
    retainProvider(hwnd, static_cast<IRawElementProviderSimple*>(provider));
    const LRESULT result = UiaReturnRawElementProvider(hwnd, wParam, lParam,
        static_cast<IRawElementProviderSimple*>(provider));
    provider->Release();
    return result;
}

LRESULT respondToOverlayUiaGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam,
                                     const OverlayUiaSemantics& semantics,
                                     std::vector<OverlayUiaAction> actions) noexcept {
    if (actions.empty()) return respondToOverlayUiaGetObject(hwnd, wParam, lParam, semantics);
    if (!hwnd || lParam != UiaRootObjectId) return 0;
    auto* provider = new (std::nothrow) ActionRootProvider(hwnd, semantics, std::move(actions));
    if (!provider) return 0;
    retainProvider(hwnd, static_cast<IRawElementProviderSimple*>(provider));
    const LRESULT result = UiaReturnRawElementProvider(hwnd, wParam, lParam,
        static_cast<IRawElementProviderSimple*>(provider));
    provider->Release();
    return result;
}

void announceOverlayUia(HWND hwnd, const OverlayUiaSemantics& semantics,
                        std::wstring_view text) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    SetWindowTextW(hwnd, std::wstring(text).c_str());
    auto* provider = new (std::nothrow) OverlayUiaProvider(hwnd, semantics);
    if (!provider) return;
    // UIA's live-region event is supported by all target builds; unlike focus
    // events, it announces the status without activating a click-through HWND.
    UiaRaiseAutomationEvent(provider, UIA_LiveRegionChangedEventId);
    provider->Release();
}

void disconnectOverlayUiaProvider(HWND hwnd) noexcept {
    disconnectProvider(hwnd);
}

}  // namespace tools3000::core::accessibility
