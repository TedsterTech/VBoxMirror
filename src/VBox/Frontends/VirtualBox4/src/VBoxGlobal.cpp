/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * VBoxGlobal class implementation
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#include "VBoxGlobal.h"

#include "VBoxDefs.h"
#include "VBoxSelectorWnd.h"
#include "VBoxConsoleWnd.h"
#include "VBoxProblemReporter.h"
#include "QIHotKeyEdit.h"
#include "QIMessageBox.h"
#include "QIDialogButtonBox.h"

#ifdef VBOX_WITH_REGISTRATION
#include "VBoxRegistrationDlg.h"
#endif

/* Qt includes */
#include <QLibraryInfo>
#include <QFileDialog>
#include <QToolTip>
#include <QTranslator>
#include <QDesktopWidget>
#include <QMutex>
#include <QToolButton>
#include <QProcess>
#include <QThread>

#ifdef Q_WS_X11
#include <QTextBrowser>
#include <QScrollBar>
#include <QX11Info>
#endif


#if defined (Q_WS_MAC)
#include <Carbon/Carbon.h> // for HIToolbox/InternetConfig
#endif

#if defined (Q_WS_WIN)
#include "shlobj.h"
#include <QEventLoop>
#endif

#if defined (Q_WS_X11)
#undef BOOL /* typedef CARD8 BOOL in Xmd.h conflicts with #define BOOL PRBool
             * in COMDefs.h. A better fix would be to isolate X11-specific
             * stuff by placing XX* helpers below to a separate source file. */
#include <X11/X.h>
#include <X11/Xmd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#define BOOL PRBool
#endif

#include <iprt/err.h>
#include <iprt/param.h>
#include <iprt/path.h>
#include <iprt/env.h>
#include <iprt/file.h>

#if defined (Q_WS_X11)
#include <iprt/mem.h>
#endif

#if defined (VBOX_GUI_DEBUG)
uint64_t VMCPUTimer::ticks_per_msec = (uint64_t) -1LL; // declared in VBoxDefs.h
#endif

// VBoxEnumerateMediaEvent
/////////////////////////////////////////////////////////////////////////////

class VBoxEnumerateMediaEvent : public QEvent
{
public:

    /** Constructs a regular enum event */
    VBoxEnumerateMediaEvent (const VBoxMedia &aMedia, int aIndex)
        : QEvent ((QEvent::Type) VBoxDefs::EnumerateMediaEventType)
        , mMedia (aMedia), mLast (false), mIndex (aIndex)
        {}
    /** Constructs the last enum event */
    VBoxEnumerateMediaEvent()
        : QEvent ((QEvent::Type) VBoxDefs::EnumerateMediaEventType)
        , mLast (true), mIndex (-1)
        {}

    /** the last enumerated media (not valid when #last is true) */
    const VBoxMedia mMedia;
    /** whether this is the last event for the given enumeration or not */
    const bool mLast;
    /** last enumerated media index (-1 when #last is true) */
    const int mIndex;
};

#if defined (Q_WS_WIN)
class VBoxShellExecuteEvent : public QEvent
{
public:

    /** Constructs a regular enum event */
    VBoxShellExecuteEvent (QThread *aThread, const QString &aURL,
                             bool aOk)
        : QEvent ((QEvent::Type) VBoxDefs::ShellExecuteEventType)
        , mThread (aThread), mURL (aURL), mOk (aOk)
        {}

    QThread *mThread;
    QString mURL;
    bool mOk;
};
#endif

// VirtualBox callback class
/////////////////////////////////////////////////////////////////////////////

class VBoxCallback : public IVirtualBoxCallback
{
public:

    VBoxCallback (VBoxGlobal &aGlobal)
        : mGlobal (aGlobal), mIsRegDlgOwner (false)
    {
#if defined (Q_OS_WIN32)
        refcnt = 0;
#endif
    }

    virtual ~VBoxCallback() {}

    NS_DECL_ISUPPORTS

#if defined (Q_OS_WIN32)
    STDMETHOD_(ULONG, AddRef)()
    {
        return ::InterlockedIncrement (&refcnt);
    }
    STDMETHOD_(ULONG, Release)()
    {
        long cnt = ::InterlockedDecrement (&refcnt);
        if (cnt == 0)
            delete this;
        return cnt;
    }
    STDMETHOD(QueryInterface) (REFIID riid , void **ppObj)
    {
        if (riid == IID_IUnknown) {
            *ppObj = this;
            AddRef();
            return S_OK;
        }
        if (riid == IID_IVirtualBoxCallback) {
            *ppObj = this;
            AddRef();
            return S_OK;
        }
        *ppObj = NULL;
        return E_NOINTERFACE;
    }
#endif

    // IVirtualBoxCallback methods

    // Note: we need to post custom events to the GUI event queue
    // instead of doing what we need directly from here because on Win32
    // these callback methods are never called on the main GUI thread.
    // Another reason to handle events asynchronously is that internally
    // most callback interface methods are called from under the initiator
    // object's lock, so accessing the initiator object (for example, reading
    // some property) directly from the callback method will definitely cause
    // a deadlock.

    STDMETHOD(OnMachineStateChange) (IN_GUIDPARAM id, MachineState_T state)
    {
        postEvent (new VBoxMachineStateChangeEvent (COMBase::ToQUuid (id),
                                                    (KMachineState) state));
        return S_OK;
    }

    STDMETHOD(OnMachineDataChange) (IN_GUIDPARAM id)
    {
        postEvent (new VBoxMachineDataChangeEvent (COMBase::ToQUuid (id)));
        return S_OK;
    }

    STDMETHOD(OnExtraDataCanChange)(IN_GUIDPARAM id,
                                    IN_BSTRPARAM key, IN_BSTRPARAM value,
                                    BSTR *error, BOOL *allowChange)
    {
        if (!error || !allowChange)
            return E_INVALIDARG;

        if (COMBase::ToQUuid (id).isNull())
        {
            /* it's a global extra data key someone wants to change */
            QString sKey = QString::fromUtf16 (key);
            QString sVal = QString::fromUtf16 (value);
            if (sKey.startsWith ("GUI/"))
            {
                if (sKey == VBoxDefs::GUI_RegistrationDlgWinID)
                {
                    if (mIsRegDlgOwner)
                    {
                        if (sVal.isEmpty() ||
                            sVal == QString ("%1").arg (static_cast<long> (vboxGlobal().mainWindow()->winId())))
                            *allowChange = TRUE;
                        else
                            *allowChange = FALSE;
                    }
                    else
                        *allowChange = TRUE;
                    return S_OK;
                }

                /* try to set the global setting to check its syntax */
                VBoxGlobalSettings gs (false /* non-null */);
                if (gs.setPublicProperty (sKey, sVal))
                {
                    /* this is a known GUI property key */
                    if (!gs)
                    {
                        /* disallow the change when there is an error*/
                        *error = SysAllocString ((const OLECHAR *)
                                                 gs.lastError().utf16());
                        *allowChange = FALSE;
                    }
                    else
                        *allowChange = TRUE;
                    return S_OK;
                }
            }
        }

        /* not interested in this key -- never disagree */
        *allowChange = TRUE;
        return S_OK;
    }

    STDMETHOD(OnExtraDataChange) (IN_GUIDPARAM id,
                                  IN_BSTRPARAM key, IN_BSTRPARAM value)
    {
        if (COMBase::ToQUuid (id).isNull())
        {
            QString sKey = QString::fromUtf16 (key);
            QString sVal = QString::fromUtf16 (value);
            if (sKey.startsWith ("GUI/"))
            {
                if (sKey == VBoxDefs::GUI_RegistrationDlgWinID)
                {
                    if (sVal.isEmpty())
                    {
                        mIsRegDlgOwner = false;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowRegDlgEvent (true));
                    }
                    else if (sVal == QString ("%1").arg (static_cast<long> (vboxGlobal().mainWindow()->winId())))
                    {
                        mIsRegDlgOwner = true;
                        QApplication::postEvent (&mGlobal, new VBoxCanShowRegDlgEvent (true));
                    }
                    else
                        QApplication::postEvent (&mGlobal, new VBoxCanShowRegDlgEvent (false));
                }
                if (sKey == "GUI/LanguageID")
                    QApplication::postEvent (&mGlobal, new VBoxChangeGUILanguageEvent (sVal));

                mMutex.lock();
                mGlobal.gset.setPublicProperty (sKey, sVal);
                mMutex.unlock();
                Assert (!!mGlobal.gset);
            }
        }
        return S_OK;
    }

    STDMETHOD(OnMediaRegistered) (IN_GUIDPARAM id, DeviceType_T type,
                                  BOOL registered)
    {
        /** @todo */
        Q_UNUSED (id);
        Q_UNUSED (type);
        Q_UNUSED (registered);
        return S_OK;
    }

    STDMETHOD(OnMachineRegistered) (IN_GUIDPARAM id, BOOL registered)
    {
        postEvent (new VBoxMachineRegisteredEvent (COMBase::ToQUuid (id),
                                                   registered));
        return S_OK;
    }

    STDMETHOD(OnSessionStateChange) (IN_GUIDPARAM id, SessionState_T state)
    {
        postEvent (new VBoxSessionStateChangeEvent (COMBase::ToQUuid (id),
                                                    (KSessionState) state));
        return S_OK;
    }

    STDMETHOD(OnSnapshotTaken) (IN_GUIDPARAM aMachineId, IN_GUIDPARAM aSnapshotId)
    {
        postEvent (new VBoxSnapshotEvent (COMBase::ToQUuid (aMachineId),
                                          COMBase::ToQUuid (aSnapshotId),
                                          VBoxSnapshotEvent::Taken));
        return S_OK;
    }

    STDMETHOD(OnSnapshotDiscarded) (IN_GUIDPARAM aMachineId, IN_GUIDPARAM aSnapshotId)
    {
        postEvent (new VBoxSnapshotEvent (COMBase::ToQUuid (aMachineId),
                                          COMBase::ToQUuid (aSnapshotId),
                                          VBoxSnapshotEvent::Discarded));
        return S_OK;
    }

    STDMETHOD(OnSnapshotChange) (IN_GUIDPARAM aMachineId, IN_GUIDPARAM aSnapshotId)
    {
        postEvent (new VBoxSnapshotEvent (COMBase::ToQUuid (aMachineId),
                                          COMBase::ToQUuid (aSnapshotId),
                                          VBoxSnapshotEvent::Changed));
        return S_OK;
    }

private:

    void postEvent (QEvent *e)
    {
        // currently, we don't post events if we are in the VM execution
        // console mode, to save some CPU ticks (so far, there was no need
        // to handle VirtualBox callback events in the execution console mode)

        if (!mGlobal.isVMConsoleProcess())
            QApplication::postEvent (&mGlobal, e);
    }

    VBoxGlobal &mGlobal;

    /** protects #OnExtraDataChange() */
    QMutex mMutex;

    bool mIsRegDlgOwner;

#if defined (Q_OS_WIN32)
private:
    long refcnt;
#endif
};

#if !defined (Q_OS_WIN32)
NS_DECL_CLASSINFO (VBoxCallback)
NS_IMPL_THREADSAFE_ISUPPORTS1_CI (VBoxCallback, IVirtualBoxCallback)
#endif

// Helpers for VBoxGlobal::getOpenFileName() & getExistingDirectory()
/////////////////////////////////////////////////////////////////////////////

#if defined Q_WS_WIN

extern void qt_enter_modal (QWidget*);
extern void qt_leave_modal (QWidget*);

static QString extractFilter (const QString &aRawFilter)
{
    static const char qt_file_dialog_filter_reg_exp[] =
        "([a-zA-Z0-9 ]*)\\(([a-zA-Z0-9_.*? +;#\\[\\]]*)\\)$";

    QString result = aRawFilter;
    QRegExp r (QString::fromLatin1 (qt_file_dialog_filter_reg_exp));
    int index = r.search (result);
    if (index >= 0)
        result = r.cap (2);
    return result.replace (QChar (' '), QChar (';'));
}

/**
 * Converts QFileDialog filter list to Win32 API filter list.
 */
static QString winFilter (const QString &aFilter)
{
    QStringList filterLst;

    if (!aFilter.isEmpty())
    {
        int i = aFilter.find (";;", 0);
        QString sep (";;");
        if (i == -1)
        {
            if (aFilter.find ("\n", 0) != -1)
            {
                sep = "\n";
                i = aFilter.find (sep, 0);
            }
        }

        filterLst = QStringList::split (sep, aFilter);
    }

    QStringList::Iterator it = filterLst.begin();
    QString winfilters;
    for (; it != filterLst.end(); ++it)
    {
        winfilters += *it;
        winfilters += QChar::Null;
        winfilters += extractFilter (*it);
        winfilters += QChar::Null;
    }
    winfilters += QChar::Null;
    return winfilters;
}

/*
 * Callback function to control the native Win32 API file dialog
 */
UINT_PTR CALLBACK OFNHookProc (HWND aHdlg, UINT aUiMsg, WPARAM aWParam, LPARAM aLParam)
{
    if (aUiMsg == WM_NOTIFY)
    {
        OFNOTIFY *notif = (OFNOTIFY*) aLParam;
        if (notif->hdr.code == CDN_TYPECHANGE)
        {
            /* locate native dialog controls */
            HWND parent = GetParent (aHdlg);
            HWND button = GetDlgItem (parent, IDOK);
            HWND textfield = ::GetDlgItem (parent, cmb13);
            if (textfield == NULL)
                textfield = ::GetDlgItem (parent, edt1);
            if (textfield == NULL)
                return FALSE;
            HWND selector = ::GetDlgItem (parent, cmb1);

            /* simulate filter change by pressing apply-key */
            int    size = 256;
            TCHAR *buffer = (TCHAR*)malloc (size);
            SendMessage (textfield, WM_GETTEXT, size, (LPARAM)buffer);
            SendMessage (textfield, WM_SETTEXT, 0, (LPARAM)"\0");
            SendMessage (button, BM_CLICK, 0, 0);
            SendMessage (textfield, WM_SETTEXT, 0, (LPARAM)buffer);
            free (buffer);

            /* make request for focus moving to filter selector combo-box */
            HWND curFocus = GetFocus();
            PostMessage (curFocus, WM_KILLFOCUS, (WPARAM)selector, 0);
            PostMessage (selector, WM_SETFOCUS, (WPARAM)curFocus, 0);
            WPARAM wParam = MAKEWPARAM (WA_ACTIVE, 0);
            PostMessage (selector, WM_ACTIVATE, wParam, (LPARAM)curFocus);
        }
    }
    return FALSE;
}

/*
 * Callback function to control the native Win32 API folders dialog
 */
static int __stdcall winGetExistDirCallbackProc (HWND hwnd, UINT uMsg,
                                                 LPARAM lParam, LPARAM lpData)
{
    if (uMsg == BFFM_INITIALIZED && lpData != 0)
    {
        QString *initDir = (QString *)(lpData);
        if (!initDir->isEmpty())
        {
            SendMessage (hwnd, BFFM_SETSELECTION, TRUE, Q_ULONG (initDir->utf16()));
            //SendMessage (hwnd, BFFM_SETEXPANDED, TRUE, Q_ULONG (initDir->utf16()));
        }
    }
    else if (uMsg == BFFM_SELCHANGED)
    {
        TCHAR path [MAX_PATH];
        SHGetPathFromIDList (LPITEMIDLIST (lParam), path);
        QString tmpStr = QString::fromUtf16 ((ushort*)path);
        if (!tmpStr.isEmpty())
            SendMessage (hwnd, BFFM_ENABLEOK, 1, 1);
        else
            SendMessage (hwnd, BFFM_ENABLEOK, 0, 0);
        SendMessage (hwnd, BFFM_SETSTATUSTEXT, 1, Q_ULONG (path));
    }
    return 0;
}

/**
 *  QEvent class to carry Win32 API native dialog's result information
 */
class OpenNativeDialogEvent : public QEvent
{
public:

    OpenNativeDialogEvent (const QString &aResult, QEvent::Type aType)
        : QEvent (aType), mResult (aResult) {}

    const QString& result() { return mResult; }

private:

    QString mResult;
};

/**
 *  QObject class reimplementation which is the target for OpenNativeDialogEvent
 *  event. It receives OpenNativeDialogEvent event from another thread,
 *  stores result information and exits event processing loop.
 */
class LoopObject : public QObject
{
public:

    LoopObject (QEvent::Type aType) : mType (aType), mResult (QString::null) {}
    const QString& result() { return mResult; }

private:

    bool event (QEvent *aEvent)
    {
        if (aEvent->type() == mType)
        {
            OpenNativeDialogEvent *ev = (OpenNativeDialogEvent*) aEvent;
            mResult = ev->result();
            qApp->eventLoop()->exitLoop();
            return true;
        }
        return QObject::event (aEvent);
    }

    QEvent::Type mType;
    QString mResult;
};

#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
/**
 *  This class is used to show a user license under linux.
 */
class VBoxLicenseViewer : public QDialog
{
    Q_OBJECT

public:

    VBoxLicenseViewer (const QString &aFilePath)
        : QDialog ()
        , mFilePath (aFilePath)
        , mLicenseText (0), mAgreeButton (0), mDisagreeButton (0)
    {
        setWindowTitle ("VirtualBox License");

#ifndef Q_WS_WIN
       /* Application icon. On Win32, it's built-in to the executable. */
        setWindowIcon (QIcon (":/VirtualBox_48px.png"));
#endif

        mLicenseText = new QTextBrowser (this);
        mAgreeButton = new QPushButton (tr ("I &Agree"), this);
        mDisagreeButton = new QPushButton (tr ("I &Disagree"), this);
        QDialogButtonBox *dbb = new QIDialogButtonBox (this);
        dbb->addButton (mAgreeButton, QDialogButtonBox::AcceptRole);
        dbb->addButton (mDisagreeButton, QDialogButtonBox::RejectRole);

        connect (mLicenseText->verticalScrollBar(), SIGNAL (valueChanged (int)),
                 SLOT (onScrollBarMoving (int)));
        connect (mAgreeButton, SIGNAL (clicked()), SLOT (accept()));
        connect (mDisagreeButton, SIGNAL (clicked()), SLOT (reject()));

        QVBoxLayout *mainLayout = new QVBoxLayout (this);
        mainLayout->setSpacing (10);
        VBoxGlobal::setLayoutMargin (mainLayout, 10);
        mainLayout->addWidget (mLicenseText);
        mainLayout->addWidget (dbb);

        mLicenseText->verticalScrollBar()->installEventFilter (this);

        resize (600, 450);
    }

public slots:

    int exec()
    {
        /* read & show the license file */
        QFile file (mFilePath);
        if (file.open (QIODevice::ReadOnly))
        {
            mLicenseText->setText (file.readAll());
            return QDialog::exec();
        }
        else
        {
            vboxProblem().cannotOpenLicenseFile (this, mFilePath);
            return QDialog::Rejected;
        }
    }

private slots:

    void onScrollBarMoving (int aValue)
    {
        if (aValue == mLicenseText->verticalScrollBar()->maximum())
            unlockButtons();
    }

    void unlockButtons()
    {
        mAgreeButton->setEnabled (true);
        mDisagreeButton->setEnabled (true);
    }

private:

    void showEvent (QShowEvent *aEvent)
    {
        QDialog::showEvent (aEvent);
        bool isScrollBarHidden = mLicenseText->verticalScrollBar()->isHidden()
                                 && !(windowState() & Qt::WindowMinimized);
        mAgreeButton->setEnabled (isScrollBarHidden);
        mDisagreeButton->setEnabled (isScrollBarHidden);
    }

    bool eventFilter (QObject *aObject, QEvent *aEvent)
    {
        switch (aEvent->type())
        {
            case QEvent::Hide:
                if (aObject == mLicenseText->verticalScrollBar())
                    /* Doesn't work on wm's like ion3 where the window starts
                     * maximized: isActiveWindow() */
                    unlockButtons();
            default:
                break;
        }
        return QDialog::eventFilter (aObject, aEvent);
    }

    QString       mFilePath;
    QTextBrowser *mLicenseText;
    QPushButton  *mAgreeButton;
    QPushButton  *mDisagreeButton;
};
#endif


// VBoxGlobal
////////////////////////////////////////////////////////////////////////////////

static bool sVBoxGlobalInited = false;
static bool sVBoxGlobalInCleanup = false;

/** @internal
 *
 *  Special routine to do VBoxGlobal cleanup when the application is being
 *  terminated. It is called before some essential Qt functionality (for
 *  instance, QThread) becomes unavailable, allowing us to use it from
 *  VBoxGlobal::cleanup() if necessary.
 */
static void vboxGlobalCleanup()
{
    Assert (!sVBoxGlobalInCleanup);
    sVBoxGlobalInCleanup = true;
    vboxGlobal().cleanup();
}

/** @internal
 *
 *  Determines the rendering mode from the argument. Sets the appropriate
 *  default rendering mode if the argumen is NULL.
 */
static VBoxDefs::RenderMode vboxGetRenderMode (const char *aModeStr)
{
    VBoxDefs::RenderMode mode = VBoxDefs::InvalidRenderMode;

#if defined (Q_WS_MAC) && defined (VBOX_GUI_USE_QUARTZ2D)
    mode = VBoxDefs::Quartz2DMode;
#elif (defined (Q_WS_WIN32) || defined (Q_WS_PM)) && defined (VBOX_GUI_USE_QIMAGE)
    mode = VBoxDefs::QImageMode;
#elif defined (Q_WS_X11) && defined (VBOX_GUI_USE_SDL)
    mode = VBoxDefs::SDLMode;
#elif defined (VBOX_GUI_USE_QIMAGE)
    mode = VBoxDefs::QImageMode;
#else
# error "Cannot determine the default render mode!"
#endif

    if (aModeStr)
    {
        if (0) ;
#if defined (VBOX_GUI_USE_QIMAGE)
        else if (::strcmp (aModeStr, "image") == 0)
            mode = VBoxDefs::QImageMode;
#endif
#if defined (VBOX_GUI_USE_SDL)
        else if (::strcmp (aModeStr, "sdl") == 0)
            mode = VBoxDefs::SDLMode;
#endif
#if defined (VBOX_GUI_USE_DDRAW)
        else if (::strcmp (aModeStr, "ddraw") == 0)
            mode = VBoxDefs::DDRAWMode;
#endif
#if defined (VBOX_GUI_USE_QUARTZ2D)
        else if (::strcmp (aModeStr, "quartz2d") == 0)
            mode = VBoxDefs::Quartz2DMode;
#endif
    }

    return mode;
}

/** @class VBoxGlobal
 *
 *  The VBoxGlobal class incapsulates the global VirtualBox data.
 *
 *  There is only one instance of this class per VirtualBox application,
 *  the reference to it is returned by the static instance() method, or by
 *  the global vboxGlobal() function, that is just an inlined shortcut.
 */

VBoxGlobal::VBoxGlobal()
    : mValid (false)
    , mSelectorWnd (NULL), mConsoleWnd (NULL)
#ifdef VBOX_WITH_REGISTRATION
    , mRegDlg (NULL)
#endif
    , media_enum_thread (NULL)
    , verString ("1.0")
    , vm_state_color (KMachineState_COUNT)
    , machineStates (KMachineState_COUNT)
    , sessionStates (KSessionState_COUNT)
    , deviceTypes (KDeviceType_COUNT)
    , storageBuses (KStorageBus_COUNT)
    , storageBusDevices (2)
    , storageBusChannels (3)
    , diskTypes (KHardDiskType_COUNT)
    , diskStorageTypes (KHardDiskStorageType_COUNT)
    , vrdpAuthTypes (KVRDPAuthType_COUNT)
    , portModeTypes (KPortMode_COUNT)
    , usbFilterActionTypes (KUSBDeviceFilterAction_COUNT)
    , audioDriverTypes (KAudioDriverType_COUNT)
    , audioControllerTypes (KAudioControllerType_COUNT)
    , networkAdapterTypes (KNetworkAdapterType_COUNT)
    , networkAttachmentTypes (KNetworkAttachmentType_COUNT)
    , clipboardTypes (KClipboardMode_COUNT)
    , ideControllerTypes (KIDEControllerType_COUNT)
    , USBDeviceStates (KUSBDeviceState_COUNT)
    , detailReportTemplatesReady (false)
{
}

//
// Public members
/////////////////////////////////////////////////////////////////////////////

/**
 *  Returns a reference to the global VirtualBox data, managed by this class.
 *
 *  The main() function of the VBox GUI must call this function soon after
 *  creating a QApplication instance but before opening any of the main windows
 *  (to let the VBoxGlobal initialization procedure use various Qt facilities),
 *  and continue execution only when the isValid() method of the returned
 *  instancereturns true, i.e. do something like:
 *
 *  @code
 *  if ( !VBoxGlobal::instance().isValid() )
 *      return 1;
 *  @endcode
 *  or
 *  @code
 *  if ( !vboxGlobal().isValid() )
 *      return 1;
 *  @endcode
 *
 *  @note Some VBoxGlobal methods can be used on a partially constructed
 *  VBoxGlobal instance, i.e. from constructors and methods called
 *  from the VBoxGlobal::init() method, which obtain the instance
 *  using this instance() call or the ::vboxGlobal() function. Currently, such
 *  methods are:
 *      #vmStateText, #vmTypeIcon, #vmTypeText, #vmTypeTextList, #vmTypeFromText.
 *
 *  @see ::vboxGlobal
 */
VBoxGlobal &VBoxGlobal::instance()
{
    static VBoxGlobal vboxGlobal_instance;

    if (!sVBoxGlobalInited)
    {
        /* check that a QApplication instance is created */
        if (qApp)
        {
            sVBoxGlobalInited = true;
            vboxGlobal_instance.init();
            /* add our cleanup handler to the list of Qt post routines */
            qAddPostRoutine (vboxGlobalCleanup);
        }
        else
            AssertMsgFailed (("Must construct a QApplication first!"));
    }
    return vboxGlobal_instance;
}

VBoxGlobal::~VBoxGlobal()
{
    qDeleteAll (vm_os_type_icons);
    qDeleteAll (mStateIcons);
    qDeleteAll (vm_state_color);
}

/**
 *  Sets the new global settings and saves them to the VirtualBox server.
 */
bool VBoxGlobal::setSettings (const VBoxGlobalSettings &gs)
{
    gs.save (mVBox);

    if (!mVBox.isOk())
    {
        vboxProblem().cannotSaveGlobalConfig (mVBox);
        return false;
    }

    /* We don't assign gs to our gset member here, because VBoxCallback
     * will update gset as necessary when new settings are successfullly
     * sent to the VirtualBox server by gs.save(). */

    return true;
}

/**
 *  Returns a reference to the main VBox VM Selector window.
 *  The reference is valid until application termination.
 *
 *  There is only one such a window per VirtualBox application.
 */
VBoxSelectorWnd &VBoxGlobal::selectorWnd()
{
#if defined (VBOX_GUI_SEPARATE_VM_PROCESS)
    AssertMsg (!vboxGlobal().isVMConsoleProcess(),
               ("Must NOT be a VM console process"));
#endif

    Assert (mValid);

    if (!mSelectorWnd)
    {
        /*
         *  We pass the address of mSelectorWnd to the constructor to let it be
         *  initialized right after the constructor is called. It is necessary
         *  to avoid recursion, since this method may be (and will be) called
         *  from the below constructor or from constructors/methods it calls.
         */
        VBoxSelectorWnd *w = new VBoxSelectorWnd (&mSelectorWnd, 0);
        Assert (w == mSelectorWnd);
        NOREF(w);
    }

    return *mSelectorWnd;
}

/**
 *  Returns a reference to the main VBox VM Console window.
 *  The reference is valid until application termination.
 *
 *  There is only one such a window per VirtualBox application.
 */
VBoxConsoleWnd &VBoxGlobal::consoleWnd()
{
#if defined (VBOX_GUI_SEPARATE_VM_PROCESS)
    AssertMsg (vboxGlobal().isVMConsoleProcess(),
               ("Must be a VM console process"));
#endif

    Assert (mValid);

    if (!mConsoleWnd)
    {
        /*
         *  We pass the address of mConsoleWnd to the constructor to let it be
         *  initialized right after the constructor is called. It is necessary
         *  to avoid recursion, since this method may be (and will be) called
         *  from the below constructor or from constructors/methods it calls.
         */
        VBoxConsoleWnd *w = new VBoxConsoleWnd (&mConsoleWnd, 0);
        Assert (w == mConsoleWnd);
        NOREF(w);
    }

    return *mConsoleWnd;
}

/**
 *  Returns the list of all guest OS type descriptions, queried from
 *  IVirtualBox.
 */
QStringList VBoxGlobal::vmGuestOSTypeDescriptions() const
{
    static QStringList list;
    if (list.empty()) {
        for (int i = 0; i < vm_os_types.count(); i++) {
            list += vm_os_types [i].GetDescription();
        }
    }
    return list;
}

/**
 *  Returns the guest OS type object corresponding to the given index.
 *  The index argument corresponds to the index in the list of OS type
 *  descriptions as returnded by #vmGuestOSTypeDescriptions().
 *
 *  If the index is invalid a null object is returned.
 */
CGuestOSType VBoxGlobal::vmGuestOSType (int aIndex) const
{
    CGuestOSType type;
    if (aIndex >= 0 && aIndex < (int) vm_os_types.count())
        type = vm_os_types.value (aIndex);
    AssertMsg (!type.isNull(), ("Index for OS type must be valid: %d", aIndex));
    return type;
}

/**
 *  Returns the index corresponding to the given guest OS type ID.
 *  The returned index corresponds to the index in the list of OS type
 *  descriptions as returnded by #vmGuestOSTypeDescriptions().
 *
 *  If the guest OS type ID is invalid, -1 is returned.
 */
int VBoxGlobal::vmGuestOSTypeIndex (const QString &aId) const
{
    for (int i = 0; i < (int) vm_os_types.count(); i++) {
        if (!vm_os_types [i].GetId().compare (aId))
            return i;
    }
    return -1;
}

/**
 *  Returns the icon corresponding to the given guest OS type ID.
 */
QPixmap VBoxGlobal::vmGuestOSTypeIcon (const QString &aId) const
{
    static const QPixmap none;
    QPixmap *p = vm_os_type_icons.value (aId);
    AssertMsg (p, ("Icon for type `%s' must be defined", aId.toLatin1().constData()));
    return p ? *p : none;
}

/**
 *  Returns the description corresponding to the given guest OS type ID.
 */
QString VBoxGlobal::vmGuestOSTypeDescription (const QString &aId) const
{
    for (int i = 0; i < (int) vm_os_types.count(); i++) {
        if (!vm_os_types [i].GetId().compare (aId))
            return vm_os_types [i].GetDescription();
    }
    return QString::null;
}

/**
 * Returns a string representation of the given channel number on the given
 * storage bus. Complementary to #toStorageChannel (KStorageBus, const
 * QString &) const.
 */
QString VBoxGlobal::toString (KStorageBus aBus, LONG aChannel) const
{
    Assert (storageBusChannels.count() == 3);
    QString channel;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            if (aChannel == 0 || aChannel == 1)
            {
                channel = storageBusChannels [aChannel];
                break;
            }

            AssertMsgFailedBreak (("Invalid channel %d\n", aChannel));
        }
        case KStorageBus_SATA:
        {
            channel = storageBusChannels [2].arg (aChannel);
            break;
        }
        default:
            AssertFailedBreak();
    }

    return channel;
}

/**
 * Returns a channel number on the given storage bus corresponding to the given
 * string representation. Complementary to #toString (KStorageBus, LONG) const.
 */
LONG VBoxGlobal::toStorageChannel (KStorageBus aBus, const QString &aChannel) const
{
    LONG channel = 0;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            QStringVector::const_iterator it =
                qFind (storageBusChannels.begin(), storageBusChannels.end(),
                       aChannel);
            AssertMsgBreak (it != storageBusChannels.end(),
                            ("No value for {%s}\n", aChannel.toLatin1().constData()));
            channel = (LONG) (it - storageBusChannels.begin());
            break;
        }
        case KStorageBus_SATA:
        {
            /// @todo use regexp to properly extract the %1 text
            QString tpl = storageBusChannels [2].arg ("");
            if (aChannel.startsWith (tpl))
            {
                channel = aChannel.right (aChannel.length() - tpl.length()).toLong();
                break;
            }

            AssertMsgFailedBreak (("Invalid channel {%s}\n", aChannel.toLatin1().constData()));
            break;
        }
        default:
            AssertFailedBreak();
    }

    return channel;
}

/**
 * Returns a string representation of the given device number of the given
 * channel on the given storage bus. Complementary to #toStorageDevice
 * (KStorageBus, LONG, const QString &) const.
 */
QString VBoxGlobal::toString (KStorageBus aBus, LONG aChannel, LONG aDevice) const
{
    NOREF (aChannel);

    Assert (storageBusDevices.count() == 2);
    QString device;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            if (aDevice == 0 || aDevice == 1)
            {
                device = storageBusDevices [aDevice];
                break;
            }

            AssertMsgFailedBreak (("Invalid device %d\n", aDevice));
        }
        case KStorageBus_SATA:
        {
            AssertMsgBreak (aDevice == 0, ("Invalid device %d\n", aDevice));
            /* always zero so far for SATA */
            break;
        }
        default:
            AssertFailedBreak();
    }

    return device;
}

/**
 * Returns a device number of the given channel on the given storage bus
 * corresponding to the given string representation. Complementary to #toString
 * (KStorageBus, LONG, LONG) const.
 */
LONG VBoxGlobal::toStorageDevice (KStorageBus aBus, LONG aChannel,
                                  const QString &aDevice) const
{
    NOREF (aChannel);

    LONG device = 0;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            QStringVector::const_iterator it =
                qFind (storageBusDevices.begin(), storageBusDevices.end(),
                       aDevice);
            AssertMsg (it != storageBusDevices.end(),
                       ("No value for {%s}", aDevice.toLatin1().constData()));
            device = (LONG) (it - storageBusDevices.begin());
            break;
        }
        case KStorageBus_SATA:
        {
            AssertMsgBreak(aDevice.isEmpty(), ("Invalid device {%s}\n", aDevice.toLatin1().constData()));
            /* always zero for SATA so far. */
            break;
        }
        default:
            AssertFailedBreak();
    }

    return device;
}

/**
 * Returns a full string representation of the given device of the given channel
 * on the given storage bus. Complementary to #toStorageParams (KStorageBus,
 * LONG, LONG) const.
 */
QString VBoxGlobal::toFullString (KStorageBus aBus, LONG aChannel,
                                  LONG aDevice) const
{
    QString device;

    switch (aBus)
    {
        case KStorageBus_IDE:
        {
            device = QString ("%1 %2 %3")
                .arg (vboxGlobal().toString (aBus))
                .arg (vboxGlobal().toString (aBus, aChannel))
                .arg (vboxGlobal().toString (aBus, aChannel, aDevice));
            break;
        }
        case KStorageBus_SATA:
        {
            /* we only have one SATA device so far which is always zero */
            device = QString ("%1 %2")
                .arg (vboxGlobal().toString (aBus))
                .arg (vboxGlobal().toString (aBus, aChannel));
            break;
        }
        default:
            AssertFailedBreak();
    }

    return device;
}

/**
 *  Returns the list of all device types (VirtualBox::DeviceType COM enum).
 */
QStringList VBoxGlobal::deviceTypeStrings() const
{
    static QStringList list;
    if (list.empty())
        for (int i = 0; i < deviceTypes.count() - 1 /* usb=n/a */; i++)
            list += deviceTypes [i];
    return list;
}

struct PortConfig
{
    const char *name;
    const ulong IRQ;
    const ulong IOBase;
};

static const PortConfig comKnownPorts[] =
{
    { "COM1", 4, 0x3F8 },
    { "COM2", 3, 0x2F8 },
    { "COM3", 4, 0x3E8 },
    { "COM4", 3, 0x2E8 },
    /* must not contain an element with IRQ=0 and IOBase=0 used to cause
     * toCOMPortName() to return the "User-defined" string for these values. */
};

static const PortConfig lptKnownPorts[] =
{
    { "LPT1", 7, 0x3BC },
    { "LPT2", 5, 0x378 },
    { "LPT3", 5, 0x278 },
    /* must not contain an element with IRQ=0 and IOBase=0 used to cause
     * toLPTPortName() to return the "User-defined" string for these values. */
};

/**
 *  Returns the list of the standard COM port names (i.e. "COMx").
 */
QStringList VBoxGlobal::COMPortNames() const
{
    QStringList list;
    for (size_t i = 0; i < ELEMENTS (comKnownPorts); ++ i)
        list << comKnownPorts [i].name;

    return list;
}

/**
 *  Returns the list of the standard LPT port names (i.e. "LPTx").
 */
QStringList VBoxGlobal::LPTPortNames() const
{
    QStringList list;
    for (size_t i = 0; i < ELEMENTS (lptKnownPorts); ++ i)
        list << lptKnownPorts [i].name;

    return list;
}

/**
 *  Returns the name of the standard COM port corresponding to the given
 *  parameters, or "User-defined" (which is also returned when both
 *  @a aIRQ and @a aIOBase are 0).
 */
QString VBoxGlobal::toCOMPortName (ulong aIRQ, ulong aIOBase) const
{
    for (size_t i = 0; i < ELEMENTS (comKnownPorts); ++ i)
        if (comKnownPorts [i].IRQ == aIRQ &&
            comKnownPorts [i].IOBase == aIOBase)
            return comKnownPorts [i].name;

    return mUserDefinedPortName;
}

/**
 *  Returns the name of the standard LPT port corresponding to the given
 *  parameters, or "User-defined" (which is also returned when both
 *  @a aIRQ and @a aIOBase are 0).
 */
QString VBoxGlobal::toLPTPortName (ulong aIRQ, ulong aIOBase) const
{
    for (size_t i = 0; i < ELEMENTS (lptKnownPorts); ++ i)
        if (lptKnownPorts [i].IRQ == aIRQ &&
            lptKnownPorts [i].IOBase == aIOBase)
            return lptKnownPorts [i].name;

    return mUserDefinedPortName;
}

/**
 *  Returns port parameters corresponding to the given standard COM name.
 *  Returns @c true on success, or @c false if the given port name is not one
 *  of the standard names (i.e. "COMx").
 */
bool VBoxGlobal::toCOMPortNumbers (const QString &aName, ulong &aIRQ,
                                   ulong &aIOBase) const
{
    for (size_t i = 0; i < ELEMENTS (comKnownPorts); ++ i)
        if (strcmp (comKnownPorts [i].name, aName.toUtf8().data()) == 0)
        {
            aIRQ = comKnownPorts [i].IRQ;
            aIOBase = comKnownPorts [i].IOBase;
            return true;
        }

    return false;
}

/**
 *  Returns port parameters corresponding to the given standard LPT name.
 *  Returns @c true on success, or @c false if the given port name is not one
 *  of the standard names (i.e. "LPTx").
 */
bool VBoxGlobal::toLPTPortNumbers (const QString &aName, ulong &aIRQ,
                                   ulong &aIOBase) const
{
    for (size_t i = 0; i < ELEMENTS (lptKnownPorts); ++ i)
        if (strcmp (lptKnownPorts [i].name, aName.toUtf8().data()) == 0)
        {
            aIRQ = lptKnownPorts [i].IRQ;
            aIOBase = lptKnownPorts [i].IOBase;
            return true;
        }

    return false;
}

/**
 *  Returns the details of the given hard disk as a single-line string
 *  to be used in the VM details view.
 *
 *  The details include the type and the virtual size of the hard disk.
 *  Note that for differencing hard disks based on immutable hard disks,
 *  the Immutable hard disk type is returned.
 *
 *  @param aHD      hard disk image (when predict = true, must be a top-level image)
 *  @param aPredict when true, the function predicts the type of the resulting
 *                  image after attaching the given image to the machine.
 *                  Otherwise, a real type of the given image is returned
 *                  (with the exception mentioned above).
 *
 *  @note The hard disk object may become uninitialized by a third party
 *  while this method is reading its properties. In this case, the method will
 *  return an empty string.
 */
QString VBoxGlobal::details (const CHardDisk &aHD, bool aPredict /* = false */,
                             bool aDoRefresh)
{
    Assert (!aPredict || aHD.GetParent().isNull());

    VBoxMedia media;
    if (!aDoRefresh)
        media = VBoxMedia (CUnknown (aHD), VBoxDefs::HD, VBoxMedia::Ok);
    else if (!findMedia (CUnknown (aHD), media))
    {
        /* media may be new and not alredy in the media list, request refresh */
        startEnumeratingMedia();
        if (!findMedia (CUnknown (aHD), media))
            AssertFailed();
    }

    CHardDisk root = aHD.GetRoot();

    // @todo *** this check is rough; if aHD becomes uninitialized, any of aHD
    // getters called afterwards will also fail. The same relates to the root
    // object (that will be aHD itself in case of non-differencing
    // disks). However, this check was added to fix a particular use case:
    // when aHD is a differencing hard disk and it happens to be discarded
    // (and uninitialized) after this method is called but before we read all
    // its properties (yes, it's possible!), the root object will be null and
    // calling methods on it will assert in the debug builds. This check seems
    // to be enough as a quick solution (fresh hard disk attachments will be
    // re-read by a state change signal after the discard operation is
    // finished, so the user will eventually see correct data), but in order
    // to solve the problem properly we need to use exceptions everywhere (or
    // check the result after every method call). See also Comment #17 and
    // below in Defect #2126.
    if (!aHD.isOk())
        return QString::null;

    QString details;

    KHardDiskType type = root.GetType();

    if (type == KHardDiskType_Normal &&
        (aHD != root || (aPredict && root.GetChildren().GetCount() != 0)))
            details = tr ("Differencing", "hard disk");
    else
        details = hardDiskTypeString (root);

    details += ", ";

    /// @todo prepend the details with the warning/error
    //  icon when not accessible

    switch (media.status)
    {
        case VBoxMedia::Unknown:
            details += tr ("<i>Checking...</i>", "hard disk");
            break;
        case VBoxMedia::Ok:
            details += formatSize (root.GetSize() * _1M);
            break;
        case VBoxMedia::Error:
        case VBoxMedia::Inaccessible:
            details += tr ("<i>Inaccessible</i>", "hard disk");
            break;
    }

    return details;
}

/**
 *  Returns the details of the given USB device as a single-line string.
 */
QString VBoxGlobal::details (const CUSBDevice &aDevice) const
{
    QString details;
    QString m = aDevice.GetManufacturer().trimmed();
    QString p = aDevice.GetProduct().trimmed();
    if (m.isEmpty() && p.isEmpty())
    {
        details =
            tr ("Unknown device %1:%2", "USB device details")
            .arg (QString().sprintf ("%04hX", aDevice.GetVendorId()))
            .arg (QString().sprintf ("%04hX", aDevice.GetProductId()));
    }
    else
    {
        if (p.toUpper().startsWith (m.toUpper()))
            details = p;
        else
            details = m + " " + p;
    }
    ushort r = aDevice.GetRevision();
    if (r != 0)
        details += QString().sprintf (" [%04hX]", r);

    return details.trimmed();
}

/**
 *  Returns the multi-line description of the given USB device.
 */
QString VBoxGlobal::toolTip (const CUSBDevice &aDevice) const
{
    QString tip =
        tr ("<nobr>Vendor ID: %1</nobr><br>"
            "<nobr>Product ID: %2</nobr><br>"
            "<nobr>Revision: %3</nobr>", "USB device tooltip")
        .arg (QString().sprintf ("%04hX", aDevice.GetVendorId()))
        .arg (QString().sprintf ("%04hX", aDevice.GetProductId()))
        .arg (QString().sprintf ("%04hX", aDevice.GetRevision()));

    QString ser = aDevice.GetSerialNumber();
    if (!ser.isEmpty())
        tip += QString (tr ("<br><nobr>Serial No. %1</nobr>", "USB device tooltip"))
                        .arg (ser);

    /* add the state field if it's a host USB device */
    CHostUSBDevice hostDev = CUnknown (aDevice);
    if (!hostDev.isNull())
    {
        tip += QString (tr ("<br><nobr>State: %1</nobr>", "USB device tooltip"))
                        .arg (vboxGlobal().toString (hostDev.GetState()));
    }

    return tip;
}

/**
 *  Puts soft hyphens after every path component in the given file name.
 *  @param fn   file name (must be a full path name)
 */
QString VBoxGlobal::prepareFileNameForHTML (const QString &fn) const
{
/// @todo (dmik) remove?
//    QString result = QDir::convertSeparators (fn);
//#ifdef Q_OS_LINUX
//    result.replace ('/', "/<font color=red>&shy;</font>");
//#else
//    result.replace ('\\', "\\<font color=red>&shy;</font>");
//#endif
//    return result;
    QFileInfo fi (fn);
    return fi.fileName();
}

/**
 *  Returns a details report on a given VM enclosed in a HTML table.
 *
 *  @param m            machine to create a report for
 *  @param isNewVM      true when called by the New VM Wizard
 *  @param withLinks    true if section titles should be hypertext links
 */
QString VBoxGlobal::detailsReport (const CMachine &m, bool isNewVM,
                                   bool withLinks, bool aDoRefresh)
{
    static const char *sTableTpl =
        "<table border=0 cellspacing=2 cellpadding=0>%1</table>";
    static const char *sSectionHrefTpl =
        "<tr><td rowspan=%1 align=left><img src='%2'></td>"
            "<td colspan=2><b><a href='%3'><nobr>%4</nobr></a></b></td></tr>"
            "%5"
        "<tr><td colspan=2><font size=1>&nbsp;</font></td></tr>";
    static const char *sSectionBoldTpl =
        "<tr><td rowspan=%1 align=left><img src='%2'></td>"
            "<td colspan=2><!-- %3 --><b><nobr>%4</nobr></b></td></tr>"
            "%5"
        "<tr><td colspan=2><font size=1>&nbsp;</font></td></tr>";
    static const char *sSectionItemTpl =
        "<tr><td width=30%><nobr>%1</nobr></td><td>%2</td></tr>";

    static QString sGeneralBasicHrefTpl, sGeneralBasicBoldTpl;
    static QString sGeneralFullHrefTpl, sGeneralFullBoldTpl;

    /* generate templates after every language change */

    if (!detailReportTemplatesReady)
    {
        detailReportTemplatesReady = true;

        QString generalItems
            = QString (sSectionItemTpl).arg (tr ("Name", "details report"), "%1")
            + QString (sSectionItemTpl).arg (tr ("OS Type", "details report"), "%2")
            + QString (sSectionItemTpl).arg (tr ("Base Memory", "details report"),
                                             tr ("<nobr>%3 MB</nobr>", "details report"));
        sGeneralBasicHrefTpl = QString (sSectionHrefTpl)
                .arg (2 + 3) /* rows */
                .arg (":/machine_16px.png", /* icon */
                      "#general", /* link */
                      tr ("General", "details report"), /* title */
                      generalItems); /* items */
        sGeneralBasicBoldTpl = QString (sSectionBoldTpl)
                .arg (2 + 3) /* rows */
                .arg (":/machine_16px.png", /* icon */
                      "#general", /* link */
                      tr ("General", "details report"), /* title */
                      generalItems); /* items */

        generalItems
           += QString (sSectionItemTpl).arg (tr ("Video Memory", "details report"),
                                             tr ("<nobr>%4 MB</nobr>", "details report"))
            + QString (sSectionItemTpl).arg (tr ("Boot Order", "details report"), "%5")
            + QString (sSectionItemTpl).arg (tr ("ACPI", "details report"), "%6")
            + QString (sSectionItemTpl).arg (tr ("IO APIC", "details report"), "%7")
            + QString (sSectionItemTpl).arg (tr ("VT-x/AMD-V", "details report"), "%8")
            + QString (sSectionItemTpl).arg (tr ("PAE/NX", "details report"), "%9");

        sGeneralFullHrefTpl = QString (sSectionHrefTpl)
            .arg (2 + 9) /* rows */
            .arg (":/machine_16px.png", /* icon */
                  "#general", /* link */
                  tr ("General", "details report"), /* title */
                  generalItems); /* items */
        sGeneralFullBoldTpl = QString (sSectionBoldTpl)
            .arg (2 + 9) /* rows */
            .arg (":/machine_16px.png", /* icon */
                  "#general", /* link */
                  tr ("General", "details report"), /* title */
                  generalItems); /* items */
    }

    /* common generated content */

    const QString &sectionTpl = withLinks
        ? sSectionHrefTpl
        : sSectionBoldTpl;

    QString hardDisks;
    {
        int rows = 2; /* including section header and footer */

        CHardDiskAttachmentEnumerator aen = m.GetHardDiskAttachments().Enumerate();
        while (aen.HasMore())
        {
            CHardDiskAttachment hda = aen.GetNext();
            CHardDisk hd = hda.GetHardDisk();
            /// @todo for the explaination of the below isOk() checks, see
            /// @todo *** in #details (const CHardDisk &, bool).
            if (hda.isOk())
            {
                CHardDisk root = hd.GetRoot();
                if (hd.isOk())
                {
                    QString src = root.GetLocation();
                    KStorageBus bus = hda.GetBus();
                    LONG channel = hda.GetChannel();
                    LONG device = hda.GetDevice();
                    hardDisks += QString (sSectionItemTpl)
                        .arg (toFullString (bus, channel, device))
                        .arg (QString ("%1 [<nobr>%2</nobr>]")
                              .arg (prepareFileNameForHTML (src))
                              .arg (details (hd, isNewVM /* predict */, aDoRefresh)));
                    ++ rows;
                }
            }
        }

        if (hardDisks.isNull())
        {
            hardDisks = QString (sSectionItemTpl)
                .arg (tr ("Not Attached", "details report (HDDs)")).arg ("");
            ++ rows;
        }

        hardDisks = sectionTpl
            .arg (rows) /* rows */
            .arg (":/hd_16px.png", /* icon */
                  "#hdds", /* link */
                  tr ("Hard Disks", "details report"), /* title */
                  hardDisks); /* items */
    }

    /* compose details report */

    const QString &generalBasicTpl = withLinks
        ? sGeneralBasicHrefTpl
        : sGeneralBasicBoldTpl;

    const QString &generalFullTpl = withLinks
        ? sGeneralFullHrefTpl
        : sGeneralFullBoldTpl;

    QString detailsReport;

    if (isNewVM)
    {
        detailsReport
            = generalBasicTpl
                .arg (m.GetName())
                .arg (vmGuestOSTypeDescription (m.GetOSTypeId()))
                .arg (m.GetMemorySize())
            + hardDisks;
    }
    else
    {
        /* boot order */
        QString bootOrder;
        for (ulong i = 1; i <= mVBox.GetSystemProperties().GetMaxBootPosition(); i++)
        {
            KDeviceType device = m.GetBootOrder (i);
            if (device == KDeviceType_Null)
                continue;
            if (!bootOrder.isEmpty())
                bootOrder += ", ";
            bootOrder += toString (device);
        }
        if (bootOrder.isEmpty())
            bootOrder = toString (KDeviceType_Null);

        CBIOSSettings biosSettings = m.GetBIOSSettings();

        /* ACPI */
        QString acpi = biosSettings.GetACPIEnabled()
            ? tr ("Enabled", "details report (ACPI)")
            : tr ("Disabled", "details report (ACPI)");

        /* IO APIC */
        QString ioapic = biosSettings.GetIOAPICEnabled()
            ? tr ("Enabled", "details report (IO APIC)")
            : tr ("Disabled", "details report (IO APIC)");

        /* VT-x/AMD-V */
        CSystemProperties props = vboxGlobal().virtualBox().GetSystemProperties();
        QString virt = m.GetHWVirtExEnabled() == KTSBool_True ?
                       tr ("Enabled", "details report (VT-x/AMD-V)") :
                       m.GetHWVirtExEnabled() == KTSBool_False ?
                       tr ("Disabled", "details report (VT-x/AMD-V)") :
                       props.GetHWVirtExEnabled() ?
                       tr ("Enabled", "details report (VT-x/AMD-V)") :
                       tr ("Disabled", "details report (VT-x/AMD-V)");

        /* PAE/NX */
        QString pae = m.GetPAEEnabled()
            ? tr ("Enabled", "details report (PAE/NX)")
            : tr ("Disabled", "details report (PAE/NX)");

        /* General + Hard Disks */
        detailsReport
            = generalFullTpl
                .arg (m.GetName())
                .arg (vmGuestOSTypeDescription (m.GetOSTypeId()))
                .arg (m.GetMemorySize())
                .arg (m.GetVRAMSize())
                .arg (bootOrder)
                .arg (acpi)
                .arg (ioapic)
                .arg (virt)
                .arg (pae)
            + hardDisks;

        QString item;

        /* DVD */
        CDVDDrive dvd = m.GetDVDDrive();
        item = QString (sSectionItemTpl);
        switch (dvd.GetState())
        {
            case KDriveState_NotMounted:
                item = item.arg (tr ("Not mounted", "details report (DVD)"), "");
                break;
            case KDriveState_ImageMounted:
            {
                CDVDImage img = dvd.GetImage();
                item = item.arg (tr ("Image", "details report (DVD)"),
                                 prepareFileNameForHTML (img.GetFilePath()));
                break;
            }
            case KDriveState_HostDriveCaptured:
            {
                CHostDVDDrive drv = dvd.GetHostDrive();
                QString drvName = drv.GetName();
                QString description = drv.GetDescription();
                QString fullName = description.isEmpty() ?
                    drvName :
                    QString ("%1 (%2)").arg (description, drvName);
                item = item.arg (tr ("Host Drive", "details report (DVD)"),
                                 fullName);
                break;
            }
            default:
                AssertMsgFailed (("Invalid DVD state: %d", dvd.GetState()));
        }
        detailsReport += sectionTpl
            .arg (2 + 1) /* rows */
            .arg (":/cd_16px.png", /* icon */
                  "#dvd", /* link */
                  tr ("CD/DVD-ROM", "details report"), /* title */
                  item); // items

        /* Floppy */
        CFloppyDrive floppy = m.GetFloppyDrive();
        item = QString (sSectionItemTpl);
        switch (floppy.GetState())
        {
            case KDriveState_NotMounted:
                item = item.arg (tr ("Not mounted", "details report (floppy)"), "");
                break;
            case KDriveState_ImageMounted:
            {
                CFloppyImage img = floppy.GetImage();
                item = item.arg (tr ("Image", "details report (floppy)"),
                                 prepareFileNameForHTML (img.GetFilePath()));
                break;
            }
            case KDriveState_HostDriveCaptured:
            {
                CHostFloppyDrive drv = floppy.GetHostDrive();
                QString drvName = drv.GetName();
                QString description = drv.GetDescription();
                QString fullName = description.isEmpty() ?
                    drvName :
                    QString ("%1 (%2)").arg (description, drvName);
                item = item.arg (tr ("Host Drive", "details report (floppy)"),
                                 fullName);
                break;
            }
            default:
                AssertMsgFailed (("Invalid floppy state: %d", floppy.GetState()));
        }
        detailsReport += sectionTpl
            .arg (2 + 1) /* rows */
            .arg (":/fd_16px.png", /* icon */
                  "#floppy", /* link */
                  tr ("Floppy", "details report"), /* title */
                  item); /* items */

        /* audio */
        {
            CAudioAdapter audio = m.GetAudioAdapter();
            int rows = audio.GetEnabled() ? 3 : 2;
            if (audio.GetEnabled())
                item = QString (sSectionItemTpl)
                       .arg (tr ("Host Driver", "details report (audio)"),
                             toString (audio.GetAudioDriver())) +
                       QString (sSectionItemTpl)
                       .arg (tr ("Controller", "details report (audio)"),
                             toString (audio.GetAudioController()));
            else
                item = QString (sSectionItemTpl)
                    .arg (tr ("Disabled", "details report (audio)"), "");

            detailsReport += sectionTpl
                .arg (rows + 1) /* rows */
                .arg (":/sound_16px.png", /* icon */
                      "#audio", /* link */
                      tr ("Audio", "details report"), /* title */
                      item); /* items */
        }
        /* network */
        {
            item = QString::null;
            ulong count = mVBox.GetSystemProperties().GetNetworkAdapterCount();
            int rows = 2; /* including section header and footer */
            for (ulong slot = 0; slot < count; slot ++)
            {
                CNetworkAdapter adapter = m.GetNetworkAdapter (slot);
                if (adapter.GetEnabled())
                {
                    KNetworkAttachmentType type = adapter.GetAttachmentType();
                    QString attType = toString (adapter.GetAdapterType())
                                      .replace (QRegExp ("\\s\\(.+\\)"), " (%1)");
                    /* don't use the adapter type string for types that have
                     * an additional symbolic network/interface name field, use
                     * this name instead */
                    if (type == KNetworkAttachmentType_HostInterface)
                        attType = attType.arg (tr ("host interface, %1",
                            "details report (network)").arg (adapter.GetHostInterface()));
                    else if (type == KNetworkAttachmentType_Internal)
                        attType = attType.arg (tr ("internal network, '%1'",
                            "details report (network)").arg (adapter.GetInternalNetwork()));
                    else
                        attType = attType.arg (vboxGlobal().toString (type));

                    item += QString (sSectionItemTpl)
                        .arg (tr ("Adapter %1", "details report (network)")
                              .arg (adapter.GetSlot()))
                        .arg (attType);
                    ++ rows;
                }
            }
            if (item.isNull())
            {
                item = QString (sSectionItemTpl)
                    .arg (tr ("Disabled", "details report (network)"), "");
                ++ rows;
            }

            detailsReport += sectionTpl
                .arg (rows) /* rows */
                .arg (":/nw_16px.png", /* icon */
                      "#network", /* link */
                      tr ("Network", "details report"), /* title */
                      item); /* items */
        }
        /* serial ports */
        {
            item = QString::null;
            ulong count = mVBox.GetSystemProperties().GetSerialPortCount();
            int rows = 2; /* including section header and footer */
            for (ulong slot = 0; slot < count; slot ++)
            {
                CSerialPort port = m.GetSerialPort (slot);
                if (port.GetEnabled())
                {
                    KPortMode mode = port.GetHostMode();
                    QString data =
                        toCOMPortName (port.GetIRQ(), port.GetIOBase()) + ", ";
                    if (mode == KPortMode_HostPipe ||
                        mode == KPortMode_HostDevice)
                        data += QString ("%1 (<nobr>%2</nobr>)")
                            .arg (vboxGlobal().toString (mode))
                            .arg (QDir::convertSeparators (port.GetPath()));
                    else
                        data += toString (mode);

                    item += QString (sSectionItemTpl)
                        .arg (tr ("Port %1", "details report (serial ports)")
                              .arg (port.GetSlot()))
                        .arg (data);
                    ++ rows;
                }
            }
            if (item.isNull())
            {
                item = QString (sSectionItemTpl)
                    .arg (tr ("Disabled", "details report (serial ports)"), "");
                ++ rows;
            }

            detailsReport += sectionTpl
                .arg (rows) /* rows */
                .arg (":/serial_port_16px.png", /* icon */
                      "#serialPorts", /* link */
                      tr ("Serial Ports", "details report"), /* title */
                      item); /* items */
        }
        /* parallel ports */
        {
            item = QString::null;
            ulong count = mVBox.GetSystemProperties().GetParallelPortCount();
            int rows = 2; /* including section header and footer */
            for (ulong slot = 0; slot < count; slot ++)
            {
                CParallelPort port = m.GetParallelPort (slot);
                if (port.GetEnabled())
                {
                    QString data =
                        toLPTPortName (port.GetIRQ(), port.GetIOBase()) +
                        QString (" (<nobr>%1</nobr>)")
                        .arg (QDir::convertSeparators (port.GetPath()));

                    item += QString (sSectionItemTpl)
                        .arg (tr ("Port %1", "details report (parallel ports)")
                              .arg (port.GetSlot()))
                        .arg (data);
                    ++ rows;
                }
            }
            if (item.isNull())
            {
                item = QString (sSectionItemTpl)
                    .arg (tr ("Disabled", "details report (parallel ports)"), "");
                ++ rows;
            }

            /* Temporary disabled */
            QString dummy = sectionTpl /* detailsReport += sectionTpl */
                .arg (rows) /* rows */
                .arg (":/parallel_port_16px.png", /* icon */
                      "#parallelPorts", /* link */
                      tr ("Parallel Ports", "details report"), /* title */
                      item); /* items */
        }
        /* USB */
        {
            CUSBController ctl = m.GetUSBController();
            if (!ctl.isNull())
            {
                /* the USB controller may be unavailable (i.e. in VirtualBox OSE) */

                if (ctl.GetEnabled())
                {
                    CUSBDeviceFilterCollection coll = ctl.GetDeviceFilters();
                    CUSBDeviceFilterEnumerator en = coll.Enumerate();
                    uint active = 0;
                    while (en.HasMore())
                        if (en.GetNext().GetActive())
                            active ++;

                    item = QString (sSectionItemTpl)
                        .arg (tr ("Device Filters", "details report (USB)"),
                              tr ("%1 (%2 active)", "details report (USB)")
                                  .arg (coll.GetCount()).arg (active));
                }
                else
                    item = QString (sSectionItemTpl)
                        .arg (tr ("Disabled", "details report (USB)"), "");

                detailsReport += sectionTpl
                    .arg (2 + 1) /* rows */
                    .arg (":/usb_16px.png", /* icon */
                          "#usb", /* link */
                          tr ("USB", "details report"), /* title */
                          item); /* items */
            }
        }
        /* Shared folders */
        {
            ulong count = m.GetSharedFolders().GetCount();
            if (count > 0)
            {
                item = QString (sSectionItemTpl)
                    .arg (tr ("Shared Folders", "details report (shared folders)"),
                          tr ("%1", "details report (shadef folders)")
                              .arg (count));
            }
            else
                item = QString (sSectionItemTpl)
                    .arg (tr ("None", "details report (shared folders)"), "");

            detailsReport += sectionTpl
                .arg (2 + 1) /* rows */
                .arg (":/shared_folder_16px.png", /* icon */
                      "#sfolders", /* link */
                      tr ("Shared Folders", "details report"), /* title */
                      item); /* items */
        }
        /* VRDP */
        {
            CVRDPServer srv = m.GetVRDPServer();
            if (!srv.isNull())
            {
                /* the VRDP server may be unavailable (i.e. in VirtualBox OSE) */

                if (srv.GetEnabled())
                    item = QString (sSectionItemTpl)
                        .arg (tr ("VRDP Server Port", "details report (VRDP)"),
                              tr ("%1", "details report (VRDP)")
                                  .arg (srv.GetPort()));
                else
                    item = QString (sSectionItemTpl)
                        .arg (tr ("Disabled", "details report (VRDP)"), "");

                detailsReport += sectionTpl
                    .arg (2 + 1) /* rows */
                    .arg (":/vrdp_16px.png", /* icon */
                          "#vrdp", /* link */
                          tr ("Remote Display", "details report"), /* title */
                          item); /* items */
            }
        }
    }

    return QString (sTableTpl). arg (detailsReport);
}

#ifdef Q_WS_X11
bool VBoxGlobal::showVirtualBoxLicense()
{
    /* get the apps doc path */
    int size = 256;
    char *buffer = (char*) RTMemTmpAlloc (size);
    RTPathAppDocs (buffer, size);
    QString path (buffer);
    RTMemTmpFree (buffer);
    QDir docDir (path);
    docDir.setFilter (QDir::Files);
    docDir.setNameFilters (QStringList ("License-*.html"));

    /* get the license files list and search for the latest license */
    QStringList filesList = docDir.entryList();
    double maxVersionNumber = 0;
    for (int index = 0; index < filesList.count(); ++ index)
    {
        QRegExp regExp ("License-([\\d\\.]+).html");
        regExp.indexIn (filesList [index]);
        QString version = regExp.cap (1);
        if (maxVersionNumber < version.toDouble())
            maxVersionNumber = version.toDouble();
    }
    if (!maxVersionNumber)
    {
        vboxProblem().cannotFindLicenseFiles (path);
        return false;
    }

    /* compose the latest license file full path */
    QString latestVersion = QString::number (maxVersionNumber);
    QString latestFilePath = docDir.absoluteFilePath (
        QString ("License-%1.html").arg (latestVersion));

    /* check for the agreed license version */
    QString licenseAgreed = virtualBox().GetExtraData (VBoxDefs::GUI_LicenseKey);
    if (licenseAgreed == latestVersion)
        return true;

    VBoxLicenseViewer licenseDialog (latestFilePath);
    bool result = licenseDialog.exec() == QDialog::Accepted;
    if (result)
        virtualBox().SetExtraData (VBoxDefs::GUI_LicenseKey, latestVersion);
    return result;
}
#endif

/**
 * Checks if any of the settings files were auto-converted and informs the user
 * if so.
 */
void VBoxGlobal::checkForAutoConvertedSettings()
{
    QString formatVersion = mVBox.GetSettingsFormatVersion();

    bool isGlobalConverted = false;
    QList <CMachine> machines;
    QString fileList;
    QString version;

    CMachineVector vec = mVBox.GetMachines2();
    for (CMachineVector::ConstIterator m = vec.begin();
         m != vec.end(); ++ m)
    {
        if (!m->GetAccessible())
            continue;

        version = m->GetSettingsFileVersion();
        if (version != formatVersion)
        {
            machines.append (*m);
            fileList += QString ("<nobr>%1&nbsp;&nbsp;&nbsp;(<i>%2</i>)</nobr><br>")
                .arg (m->GetSettingsFilePath())
                .arg (version);
        }
    }

    version = mVBox.GetSettingsFileVersion();
    if (version != formatVersion)
    {
        isGlobalConverted = true;
        fileList += QString ("<nobr>%1&nbsp;&nbsp;&nbsp;(<i>%2</i>)</nobr><br>")
            .arg (mVBox.GetSettingsFilePath())
            .arg (version);
    }


    if (!fileList.isNull())
    {
        int rc = vboxProblem()
            .warnAboutAutoConvertedSettings (formatVersion, fileList);

        if (rc == QIMessageBox::No || rc == QIMessageBox::Yes)
        {
            /* backup (optionally) and save all settings files
             * (QIMessageBox::No = Backup, QIMessageBox::Yes = Save) */

            foreach (CMachine m, machines)
            {
                CSession session = openSession (m.GetId());
                if (!session.isNull())
                {
                    CMachine sm = session.GetMachine();
                    if (rc == QIMessageBox::No)
                        sm.SaveSettingsWithBackup();
                    else
                        sm.SaveSettings();

                    if (!sm.isOk())
                        vboxProblem().cannotSaveMachineSettings (sm);
                    session.Close();
                }
            }

            if (isGlobalConverted)
            {
                if (rc == QIMessageBox::No)
                    mVBox.SaveSettingsWithBackup();
                else
                    mVBox.SaveSettings();

                if (!mVBox.isOk())
                    vboxProblem().cannotSaveGlobalSettings (mVBox);
            }
        }
    }
}

/**
 *  Opens a direct session for a machine with the given ID.
 *  This method does user-friendly error handling (display error messages, etc.).
 *  and returns a null CSession object in case of any error.
 *  If this method succeeds, don't forget to close the returned session when
 *  it is no more necessary.
 *
 *  @param aId          Machine ID.
 *  @param aExisting    @c true to open an existing session with the machine
 *                      which is already running, @c false to open a new direct
 *                      session.
 */
CSession VBoxGlobal::openSession (const QUuid &aId, bool aExisting /* = false */)
{
    CSession session;
    session.createInstance (CLSID_Session);
    if (session.isNull())
    {
        vboxProblem().cannotOpenSession (session);
        return session;
    }

    aExisting ? mVBox.OpenExistingSession (session, aId) :
                mVBox.OpenSession (session, aId);

    if (!mVBox.isOk())
    {
        CMachine machine = CVirtualBox (mVBox).GetMachine (aId);
        vboxProblem().cannotOpenSession (mVBox, machine);
        session.detach();
    }

    return session;
}

/**
 *  Starts a machine with the given ID.
 */
bool VBoxGlobal::startMachine (const QUuid &id)
{
    AssertReturn (mValid, false);

    CSession session = vboxGlobal().openSession (id);
    if (session.isNull())
        return false;

    return consoleWnd().openView (session);
}

/**
 *  Appends the disk object and all its children to the media list.
 */
static
void addMediaToList (VBoxMediaList &aList,
                     const CUnknown &aDisk,
                     VBoxDefs::DiskType aType)
{
    VBoxMedia media (aDisk, aType, VBoxMedia::Unknown);
    aList += media;
    /* append all vdi children */
    if (aType == VBoxDefs::HD)
    {
        CHardDisk hd = aDisk;
        CHardDiskEnumerator enumerator = hd.GetChildren().Enumerate();
        while (enumerator.HasMore())
        {
            CHardDisk subHd = enumerator.GetNext();
            addMediaToList (aList, CUnknown (subHd), VBoxDefs::HD);
        }
    }
}

/**
 *  Starts a thread that asynchronously enumerates all currently registered
 *  media, checks for its accessibility and posts VBoxEnumerateMediaEvent
 *  events to the VBoxGlobal object until all media is enumerated.
 *
 *  If the enumeration is already in progress, no new thread is started.
 *
 *  @sa #currentMediaList()
 *  @sa #isMediaEnumerationStarted()
 */
void VBoxGlobal::startEnumeratingMedia()
{
    Assert (mValid);

    /* check if already started but not yet finished */
    if (media_enum_thread)
        return;

    /* ignore the request during application termination */
    if (sVBoxGlobalInCleanup)
        return;

    /* composes a list of all currently known media & their children */
    media_list.clear();
    {
        CHardDiskEnumerator enHD = mVBox.GetHardDisks().Enumerate();
        while (enHD.HasMore())
            addMediaToList (media_list, CUnknown (enHD.GetNext()), VBoxDefs::HD);

        CDVDImageEnumerator enCD = mVBox.GetDVDImages().Enumerate();
        while (enCD.HasMore())
            addMediaToList (media_list, CUnknown (enCD.GetNext()), VBoxDefs::CD);

        CFloppyImageEnumerator enFD = mVBox.GetFloppyImages().Enumerate();
        while (enFD.HasMore())
            addMediaToList (media_list, CUnknown (enFD.GetNext()), VBoxDefs::FD);
    }

    /* enumeration thread class */
    class Thread : public QThread
    {
    public:

        Thread (const VBoxMediaList &aList) : mList (aList) {}

        virtual void run()
        {
            LogFlow (("MediaEnumThread started.\n"));
            COMBase::InitializeCOM();

            CVirtualBox mVBox = vboxGlobal().virtualBox();
            QObject *target = &vboxGlobal();

            /* enumerating list */
            int index = 0;
            VBoxMediaList::const_iterator it;
            for (it = mList.begin();
                 it != mList.end() && !sVBoxGlobalInCleanup;
                 ++ it, ++ index)
            {
                VBoxMedia media = *it;
                switch (media.type)
                {
                    case VBoxDefs::HD:
                    {
                        CHardDisk hd = media.disk;
                        media.status =
                            hd.GetAccessible() == TRUE ? VBoxMedia::Ok :
                            hd.isOk() ? VBoxMedia::Inaccessible :
                            VBoxMedia::Error;
                        /* assign back to store error info if any */
                        media.disk = hd;
                        if (media.status == VBoxMedia::Inaccessible)
                        {
                            QUuid machineId = hd.GetMachineId();
                            if (!machineId.isNull())
                            {
                                CMachine machine = mVBox.GetMachine (machineId);
                                if (!machine.isNull() && (machine.GetState() >= KMachineState_Running))
                                    media.status = VBoxMedia::Ok;
                            }
                        }
                        QApplication::postEvent (target,
                            new VBoxEnumerateMediaEvent (media, index));
                        break;
                    }
                    case VBoxDefs::CD:
                    {
                        CDVDImage cd = media.disk;
                        media.status =
                            cd.GetAccessible() == TRUE ? VBoxMedia::Ok :
                            cd.isOk() ? VBoxMedia::Inaccessible :
                            VBoxMedia::Error;
                        /* assign back to store error info if any */
                        media.disk = cd;
                        QApplication::postEvent (target,
                            new VBoxEnumerateMediaEvent (media, index));
                        break;
                    }
                    case VBoxDefs::FD:
                    {
                        CFloppyImage fd = media.disk;
                        media.status =
                            fd.GetAccessible() == TRUE ? VBoxMedia::Ok :
                            fd.isOk() ? VBoxMedia::Inaccessible :
                            VBoxMedia::Error;
                        /* assign back to store error info if any */
                        media.disk = fd;
                        QApplication::postEvent (target,
                            new VBoxEnumerateMediaEvent (media, index));
                        break;
                    }
                    default:
                    {
                        AssertMsgFailed (("Invalid aMedia type\n"));
                        break;
                    }
                }
            }

            /* post the last message to indicate the end of enumeration */
            if (!sVBoxGlobalInCleanup)
                QApplication::postEvent (target, new VBoxEnumerateMediaEvent());

            COMBase::CleanupCOM();
            LogFlow (("MediaEnumThread finished.\n"));
        }

    private:

        const VBoxMediaList &mList;
    };

    media_enum_thread = new Thread (media_list);
    AssertReturnVoid (media_enum_thread);

    /* emit mediaEnumStarted() after we set media_enum_thread to != NULL
     * to cause isMediaEnumerationStarted() to return TRUE from slots */
    emit mediaEnumStarted();

    media_enum_thread->start();
}

/**
 *  Adds a new media to the current media list.
 *  @note Currently, this method does nothing but emits the mediaAdded() signal.
 *        Later, it will be used to synchronize the current media list with
 *        the actial media list on the server after a single media opetartion
 *        performed from within one of our UIs.
 *  @sa #currentMediaList()
 */
void VBoxGlobal::addMedia (const VBoxMedia &aMedia)
{
    emit mediaAdded (aMedia);
}

/**
 *  Updates the media in the current media list.
 *  @note Currently, this method does nothing but emits the mediaUpdated() signal.
 *        Later, it will be used to synchronize the current media list with
 *        the actial media list on the server after a single media opetartion
 *        performed from within one of our UIs.
 *  @sa #currentMediaList()
 */
void VBoxGlobal::updateMedia (const VBoxMedia &aMedia)
{
    emit mediaUpdated (aMedia);
}

/**
 *  Removes the media from the current media list.
 *  @note Currently, this method does nothing but emits the mediaRemoved() signal.
 *        Later, it will be used to synchronize the current media list with
 *        the actial media list on the server after a single media opetartion
 *        performed from within one of our UIs.
 *  @sa #currentMediaList()
 */
void VBoxGlobal::removeMedia (VBoxDefs::DiskType aType, const QUuid &aId)
{
    emit mediaRemoved (aType, aId);
}

/**
 *  Searches for a VBoxMedia object representing the given COM media object.
 *
 *  @return true if found and false otherwise.
 */
bool VBoxGlobal::findMedia (const CUnknown &aObj, VBoxMedia &aMedia) const
{
    for (VBoxMediaList::ConstIterator it = media_list.begin();
         it != media_list.end(); ++ it)
    {
        if ((*it).disk == aObj)
        {
            aMedia = (*it);
            return true;
        }
    }

    return false;
}

/**
 *  Native language name of the currently installed translation.
 *  Returns "English" if no translation is installed
 *  or if the translation file is invalid.
 */
QString VBoxGlobal::languageName() const
{

    return qApp->translate ("@@@", "English",
                            "Native language name");
}

/**
 *  Native language country name of the currently installed translation.
 *  Returns "--" if no translation is installed or if the translation file is
 *  invalid, or if the language is independent on the country.
 */
QString VBoxGlobal::languageCountry() const
{
    return qApp->translate ("@@@", "--",
                            "Native language country name "
                            "(empty if this language is for all countries)");
}

/**
 *  Language name of the currently installed translation, in English.
 *  Returns "English" if no translation is installed
 *  or if the translation file is invalid.
 */
QString VBoxGlobal::languageNameEnglish() const
{

    return qApp->translate ("@@@", "English",
                            "Language name, in English");
}

/**
 *  Language country name of the currently installed translation, in English.
 *  Returns "--" if no translation is installed or if the translation file is
 *  invalid, or if the language is independent on the country.
 */
QString VBoxGlobal::languageCountryEnglish() const
{
    return qApp->translate ("@@@", "--",
                            "Language country name, in English "
                            "(empty if native country name is empty)");
}

/**
 *  Comma-separated list of authors of the currently installed translation.
 *  Returns "Sun Microsystems, Inc." if no translation is installed or if the
 *  translation file is invalid, or if the translation is supplied by Sun
 *  Microsystems, inc.
 */
QString VBoxGlobal::languageTranslators() const
{
    return qApp->translate ("@@@", "Sun Microsystems, Inc.",
                            "Comma-separated list of translators");
}

/**
 *  Changes the language of all global string constants according to the
 *  currently installed translations tables.
 */
void VBoxGlobal::retranslateUi()
{
    machineStates [KMachineState_PoweredOff] =  tr ("Powered Off", "MachineState");
    machineStates [KMachineState_Saved] =       tr ("Saved", "MachineState");
    machineStates [KMachineState_Aborted] =     tr ("Aborted", "MachineState");
    machineStates [KMachineState_Running] =     tr ("Running", "MachineState");
    machineStates [KMachineState_Paused] =      tr ("Paused", "MachineState");
    machineStates [KMachineState_Stuck] =       tr ("Stuck", "MachineState");
    machineStates [KMachineState_Starting] =    tr ("Starting", "MachineState");
    machineStates [KMachineState_Stopping] =    tr ("Stopping", "MachineState");
    machineStates [KMachineState_Saving] =      tr ("Saving", "MachineState");
    machineStates [KMachineState_Restoring] =   tr ("Restoring", "MachineState");
    machineStates [KMachineState_Discarding] =  tr ("Discarding", "MachineState");

    sessionStates [KSessionState_Closed] =      tr ("Closed", "SessionState");
    sessionStates [KSessionState_Open] =        tr ("Open", "SessionState");
    sessionStates [KSessionState_Spawning] =    tr ("Spawning", "SessionState");
    sessionStates [KSessionState_Closing] =     tr ("Closing", "SessionState");

    deviceTypes [KDeviceType_Null] =            tr ("None", "DeviceType");
    deviceTypes [KDeviceType_Floppy] =          tr ("Floppy", "DeviceType");
    deviceTypes [KDeviceType_DVD] =             tr ("CD/DVD-ROM", "DeviceType");
    deviceTypes [KDeviceType_HardDisk] =        tr ("Hard Disk", "DeviceType");
    deviceTypes [KDeviceType_Network] =         tr ("Network", "DeviceType");
    deviceTypes [KDeviceType_USB] =             tr ("USB", "DeviceType");
    deviceTypes [KDeviceType_SharedFolder] =    tr ("Shared Folder", "DeviceType");

    storageBuses [KStorageBus_IDE] =
        tr ("IDE", "StorageBus");
    storageBuses [KStorageBus_SATA] =
        tr ("SATA", "StorageBus");

    Assert (storageBusChannels.count() == 3);
    storageBusChannels [0] =
        tr ("Primary", "StorageBusChannel");
    storageBusChannels [1] =
        tr ("Secondary", "StorageBusChannel");
    storageBusChannels [2] =
        tr ("Port %1", "StorageBusChannel");

    Assert (storageBusDevices.count() == 2);
    storageBusDevices [0] = tr ("Master", "StorageBusDevice");
    storageBusDevices [1] = tr ("Slave", "StorageBusDevice");

    diskTypes [KHardDiskType_Normal] =
        tr ("Normal", "DiskType");
    diskTypes [KHardDiskType_Immutable] =
        tr ("Immutable", "DiskType");
    diskTypes [KHardDiskType_Writethrough] =
        tr ("Writethrough", "DiskType");

    diskStorageTypes [KHardDiskStorageType_VirtualDiskImage] =
        tr ("Virtual Disk Image", "DiskStorageType");
    diskStorageTypes [KHardDiskStorageType_ISCSIHardDisk] =
        tr ("iSCSI", "DiskStorageType");
    diskStorageTypes [KHardDiskStorageType_VMDKImage] =
        tr ("VMDK Image", "DiskStorageType");
    diskStorageTypes [KHardDiskStorageType_CustomHardDisk] =
        tr ("Custom Hard Disk", "DiskStorageType");
    diskStorageTypes [KHardDiskStorageType_VHDImage] =
        tr ("VHD Image", "DiskStorageType");

    vrdpAuthTypes [KVRDPAuthType_Null] =
        tr ("Null", "VRDPAuthType");
    vrdpAuthTypes [KVRDPAuthType_External] =
        tr ("External", "VRDPAuthType");
    vrdpAuthTypes [KVRDPAuthType_Guest] =
        tr ("Guest", "VRDPAuthType");

    portModeTypes [KPortMode_Disconnected] =
        tr ("Disconnected", "PortMode");
    portModeTypes [KPortMode_HostPipe] =
        tr ("Host Pipe", "PortMode");
    portModeTypes [KPortMode_HostDevice] =
        tr ("Host Device", "PortMode");

    usbFilterActionTypes [KUSBDeviceFilterAction_Ignore] =
        tr ("Ignore", "USBFilterActionType");
    usbFilterActionTypes [KUSBDeviceFilterAction_Hold] =
        tr ("Hold", "USBFilterActionType");

    audioDriverTypes [KAudioDriverType_Null] =
        tr ("Null Audio Driver", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_WinMM] =
        tr ("Windows Multimedia", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_SolAudio] =
        tr ("Solaris Audio", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_OSS] =
        tr ("OSS Audio Driver", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_ALSA] =
        tr ("ALSA Audio Driver", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_DirectSound] =
        tr ("Windows DirectSound", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_CoreAudio] =
        tr ("CoreAudio", "AudioDriverType");
    audioDriverTypes [KAudioDriverType_Pulse] =
        tr ("PulseAudio", "AudioDriverType");

    audioControllerTypes [KAudioControllerType_AC97] =
        tr ("ICH AC97", "AudioControllerType");
    audioControllerTypes [KAudioControllerType_SB16] =
        tr ("SoundBlaster 16", "AudioControllerType");

    networkAdapterTypes [KNetworkAdapterType_Am79C970A] =
        tr ("PCnet-PCI II (Am79C970A)", "NetworkAdapterType");
    networkAdapterTypes [KNetworkAdapterType_Am79C973] =
        tr ("PCnet-FAST III (Am79C973)", "NetworkAdapterType");
    networkAdapterTypes [KNetworkAdapterType_I82540EM] =
        tr ("Intel PRO/1000 MT Desktop (82540EM)", "NetworkAdapterType");
    networkAdapterTypes [KNetworkAdapterType_I82543GC] =
        tr ("Intel PRO/1000 T Server (82543GC)", "NetworkAdapterType");

    networkAttachmentTypes [KNetworkAttachmentType_Null] =
        tr ("Not attached", "NetworkAttachmentType");
    networkAttachmentTypes [KNetworkAttachmentType_NAT] =
        tr ("NAT", "NetworkAttachmentType");
    networkAttachmentTypes [KNetworkAttachmentType_HostInterface] =
        tr ("Host Interface", "NetworkAttachmentType");
    networkAttachmentTypes [KNetworkAttachmentType_Internal] =
        tr ("Internal Network", "NetworkAttachmentType");

    clipboardTypes [KClipboardMode_Disabled] =
        tr ("Disabled", "ClipboardType");
    clipboardTypes [KClipboardMode_HostToGuest] =
        tr ("Host To Guest", "ClipboardType");
    clipboardTypes [KClipboardMode_GuestToHost] =
        tr ("Guest To Host", "ClipboardType");
    clipboardTypes [KClipboardMode_Bidirectional] =
        tr ("Bidirectional", "ClipboardType");

    ideControllerTypes [KIDEControllerType_PIIX3] =
        tr ("PIIX3", "IDEControllerType");
    ideControllerTypes [KIDEControllerType_PIIX4] =
        tr ("PIIX4", "IDEControllerType");

    USBDeviceStates [KUSBDeviceState_NotSupported] =
        tr ("Not supported", "USBDeviceState");
    USBDeviceStates [KUSBDeviceState_Unavailable] =
        tr ("Unavailable", "USBDeviceState");
    USBDeviceStates [KUSBDeviceState_Busy] =
        tr ("Busy", "USBDeviceState");
    USBDeviceStates [KUSBDeviceState_Available] =
        tr ("Available", "USBDeviceState");
    USBDeviceStates [KUSBDeviceState_Held] =
        tr ("Held", "USBDeviceState");
    USBDeviceStates [KUSBDeviceState_Captured] =
        tr ("Captured", "USBDeviceState");

    mUserDefinedPortName = tr ("User-defined", "serial port");

    detailReportTemplatesReady = false;

#if defined (Q_WS_PM) || defined (Q_WS_X11)
    /* As PM and X11 do not (to my knowledge) have functionality for providing
     * human readable key names, we keep a table of them, which must be
     * updated when the language is changed. */
#warning port me
    QIHotKeyEdit::retranslateUi();
#endif
}

// public static stuff
////////////////////////////////////////////////////////////////////////////////

/* static */
bool VBoxGlobal::isDOSType (const QString &aOSTypeId)
{
    if (aOSTypeId.left (3) == "dos" ||
        aOSTypeId.left (3) == "win" ||
        aOSTypeId.left (3) == "os2")
        return true;

    return false;
}

/**
 *  Sets the QLabel background and frame colors according tho the pixmap
 *  contents. The bottom right pixel of the label pixmap defines the
 *  background color of the label, the top right pixel defines the color of
 *  the one-pixel frame around it. This function also sets the alignment of
 *  the pixmap to AlignVTop (to correspond to the color choosing logic).
 *
 *  This method is useful to provide nice scaling of pixmal labels without
 *  scaling pixmaps themselves. To see th eeffect, the size policy of the
 *  label in the corresponding direction (vertical, for now) should be set to
 *  something like MinimumExpanding.
 *
 *  @todo Parametrize corners to select pixels from and set the alignment
 *  accordingly.
 */
/* static */
void VBoxGlobal::adoptLabelPixmap (QLabel *aLabel)
{
    AssertReturnVoid (aLabel);

    aLabel->setAlignment (Qt::AlignTop);
    aLabel->setFrameShape (QFrame::Box);
    aLabel->setFrameShadow (QFrame::Plain);

    const QPixmap *pix = aLabel->pixmap();
    QImage img = pix->toImage();
    QRgb rgbBack = img.pixel (img.width() - 1, img.height() - 1);
    QRgb rgbFrame = img.pixel (img.width() - 1, 0);

    QPalette pal = aLabel->palette();
    pal.setColor (QPalette::Window, rgbBack);
    pal.setColor (QPalette::WindowText, rgbFrame);
    aLabel->setPalette (pal);
}

extern const char *gVBoxLangSubDir = "/nls4";
extern const char *gVBoxLangFileBase = "VirtualBox_";
extern const char *gVBoxLangFileExt = ".qm";
extern const char *gVBoxLangIDRegExp = "(([a-z]{2})(?:_([A-Z]{2}))?)|(C)";
extern const char *gVBoxBuiltInLangName   = "C";

class VBoxTranslator : public QTranslator
{
public:

    VBoxTranslator (QObject *aParent = 0)
        : QTranslator (aParent) {}

    bool loadFile (const QString &aFileName)
    {
        QFile file (aFileName);
        if (!file.open (QIODevice::ReadOnly))
            return false;
        mData = file.readAll();
        return load ((uchar*) mData.data(), mData.size());
    }

private:

    QByteArray mData;
};

static VBoxTranslator *sTranslator = 0;
static QString sLoadedLangId = gVBoxBuiltInLangName;

/**
 *  Returns the loaded (active) language ID.
 *  Note that it may not match with VBoxGlobalSettings::languageId() if the
 *  specified language cannot be loaded.
 *  If the built-in language is active, this method returns "C".
 *
 *  @note "C" is treated as the built-in language for simplicity -- the C
 *  locale is used in unix environments as a fallback when the requested
 *  locale is invalid. This way we don't need to process both the "built_in"
 *  language and the "C" language (which is a valid environment setting)
 *  separately.
 */
/* static */
QString VBoxGlobal::languageId()
{
    return sLoadedLangId;
}

/**
 *  Loads the language by language ID.
 *
 *  @param aLangId Language ID in in form of xx_YY. QString::null means the
 *                 system default language.
 */
/* static */
void VBoxGlobal::loadLanguage (const QString &aLangId)
{
    QString langId = aLangId.isNull() ?
        VBoxGlobal::systemLanguageId() : aLangId;
    QString languageFileName;
    QString selectedLangId = gVBoxBuiltInLangName;

    char szNlsPath[RTPATH_MAX];
    int rc;

    rc = RTPathAppPrivateNoArch(szNlsPath, sizeof(szNlsPath));
    Assert(RT_SUCCESS(rc));

    QString nlsPath = QString(szNlsPath) + gVBoxLangSubDir;
    QDir nlsDir (nlsPath);

    Assert (!langId.isEmpty());
    if (!langId.isEmpty() && langId != gVBoxBuiltInLangName)
    {
        QRegExp regExp (gVBoxLangIDRegExp);
        int pos = regExp.indexIn (langId);
        /* the language ID should match the regexp completely */
        AssertReturnVoid (pos == 0);

        QString lang = regExp.cap (2);

        if (nlsDir.exists (gVBoxLangFileBase + langId + gVBoxLangFileExt))
        {
            languageFileName = nlsDir.absoluteFilePath (gVBoxLangFileBase + langId +
                                                        gVBoxLangFileExt);
            selectedLangId = langId;
        }
        else if (nlsDir.exists (gVBoxLangFileBase + lang + gVBoxLangFileExt))
        {
            languageFileName = nlsDir.absoluteFilePath (gVBoxLangFileBase + lang +
                                                        gVBoxLangFileExt);
            selectedLangId = lang;
        }
        else
        {
            /* Never complain when the default language is requested.  In any
             * case, if no explicit language file exists, we will simply
             * fall-back to English (built-in). */
            if (!aLangId.isNull())
                vboxProblem().cannotFindLanguage (langId, nlsPath);
            /* selectedLangId remains built-in here */
            AssertReturnVoid (selectedLangId == gVBoxBuiltInLangName);
        }
    }

    /* delete the old translator if there is one */
    if (sTranslator)
    {
        /* QTranslator destructor will call qApp->removeTranslator() for
         * us. It will also delete all its child translations we attach to it
         * below, so we don't have to care about them specially. */
        delete sTranslator;
    }

    /* load new language files */
    sTranslator = new VBoxTranslator (qApp);
    Assert (sTranslator);
    bool loadOk = true;
    if (sTranslator)
    {
        if (selectedLangId != gVBoxBuiltInLangName)
        {
            Assert (!languageFileName.isNull());
            loadOk = sTranslator->loadFile (languageFileName);
        }
        /* we install the translator in any case: on failure, this will
         * activate an empty translator that will give us English
         * (built-in) */
        qApp->installTranslator (sTranslator);
    }
    else
        loadOk = false;

    if (loadOk)
        sLoadedLangId = selectedLangId;
    else
    {
        vboxProblem().cannotLoadLanguage (languageFileName);
        sLoadedLangId = gVBoxBuiltInLangName;
    }

    /* Try to load the corresponding Qt translation */
    if (sLoadedLangId != gVBoxBuiltInLangName)
    {
#ifdef Q_OS_UNIX
        /* We use system installations of Qt on Linux systems, so first, try
         * to load the Qt translation from the system location. */
        languageFileName = QLibraryInfo::location(QLibraryInfo::TranslationsPath) + "/qt_" +
                           sLoadedLangId + gVBoxLangFileExt;
        QTranslator *qtSysTr = new QTranslator (sTranslator);
        Assert (qtSysTr);
        if (qtSysTr && qtSysTr->load (languageFileName))
            qApp->installTranslator (qtSysTr);
        /* Note that the Qt translation supplied by Sun is always loaded
         * afterwards to make sure it will take precedence over the system
         * translation (it may contain more decent variants of translation
         * that better correspond to VirtualBox UI). We need to load both
         * because a newer version of Qt may be installed on the user computer
         * and the Sun version may not fully support it. We don't do it on
         * Win32 because we supply a Qt library there and therefore the
         * Sun translation is always the best one. */
#endif
        languageFileName =  nlsDir.absoluteFilePath (QString ("qt_") +
                                                     sLoadedLangId +
                                                     gVBoxLangFileExt);
        QTranslator *qtTr = new QTranslator (sTranslator);
        Assert (qtTr);
        if (qtTr && (loadOk = qtTr->load (languageFileName)))
            qApp->installTranslator (qtTr);
        /* The below message doesn't fit 100% (because it's an additonal
         * language and the main one won't be reset to built-in on failure)
         * but the load failure is so rare here that it's not worth a separate
         * message (but still, having something is better than having none) */
        if (!loadOk && !aLangId.isNull())
            vboxProblem().cannotLoadLanguage (languageFileName);
    }
}

/* static */
QIcon VBoxGlobal::iconSet (const char *aNormal,
                           const char *aDisabled /* = NULL */,
                           const char *aActive /* = NULL */)
{
    QIcon iconSet;

    iconSet.addFile (aNormal, QSize(),
                     QIcon::Normal);
    if (aDisabled != NULL)
        iconSet.addFile (aDisabled, QSize(),
                         QIcon::Disabled);
    if (aActive != NULL)
        iconSet.addFile (aActive, QSize(),
                         QIcon::Active);
    return iconSet;
}

/* static */
QIcon VBoxGlobal::
iconSetEx (const char *aNormal, const char *aSmallNormal,
           const char *aDisabled /* = NULL */,
           const char *aSmallDisabled /* = NULL */,
           const char *aActive /* = NULL */,
           const char *aSmallActive /* = NULL */)
{
    QIcon iconSet;

    iconSet.addFile (aNormal, QSize(), QIcon::Normal);
    iconSet.addFile (aSmallNormal, QSize(), QIcon::Normal);
    if (aSmallDisabled != NULL)
    {
        iconSet.addFile (aDisabled, QSize(), QIcon::Disabled);
        iconSet.addFile (aSmallDisabled, QSize(), QIcon::Disabled);
    }
    if (aSmallActive != NULL)
    {
        iconSet.addFile (aActive, QSize(), QIcon::Active);
        iconSet.addFile (aSmallActive, QSize(), QIcon::Active);
    }

    return iconSet;
}

/**
 *  Replacement for QToolButton::setTextLabel() that handles the shortcut
 *  letter (if it is present in the argument string) as if it were a setText()
 *  call: the shortcut letter is used to automatically assign an "Alt+<letter>"
 *  accelerator key sequence to the given tool button.
 *
 *  @note This method preserves the icon set if it was assigned before. Only
 *  the text label and the accelerator are changed.
 *
 *  @param aToolButton  Tool button to set the text label on.
 *  @param aTextLabel   Text label to set.
 */
/* static */
void VBoxGlobal::setTextLabel (QToolButton *aToolButton,
                               const QString &aTextLabel)
{
    AssertReturnVoid (aToolButton != NULL);

    /* remember the icon set as setText() will kill it */
    QIcon iset = aToolButton->icon();
    /* re-use the setText() method to detect and set the accelerator */
    aToolButton->setText (aTextLabel);
    QKeySequence accel = aToolButton->shortcut();
    aToolButton->setText (aTextLabel);
    aToolButton->setIcon (iset);
    /* set the accel last as setIconSet() would kill it */
    aToolButton->setShortcut (accel);
}

/**
 *  Ensures that the given rectangle \a aRect is fully contained within the
 *  rectangle \a aBoundRect by moving \a aRect if necessary. If \a aRect is
 *  larger than \a aBoundRect, its top left corner is simply aligned with the
 *  top left corner of \a aRect and, if \a aCanResize is true, \a aRect is
 *  shrinked to become fully visible.
 */
/* static */
QRect VBoxGlobal::normalizeGeometry (const QRect &aRect, const QRect &aBoundRect,
                                     bool aCanResize /* = true */)
{
    QRect fr = aRect;

    /* make the bottom right corner visible */
    int rd = aBoundRect.right() - fr.right();
    int bd = aBoundRect.bottom() - fr.bottom();
    fr.translate (rd < 0 ? rd : 0, bd < 0 ? bd : 0);

    /* ensure the top left corner is visible */
    int ld = fr.left() - aBoundRect.left();
    int td = fr.top() - aBoundRect.top();
    fr.translate (ld < 0 ? -ld : 0, td < 0 ? -td : 0);

    if (aCanResize)
    {
        /* adjust the size to make the rectangle fully contained */
        rd = aBoundRect.right() - fr.right();
        bd = aBoundRect.bottom() - fr.bottom();
        if (rd < 0)
            fr.setRight (fr.right() + rd);
        if (bd < 0)
            fr.setBottom (fr.bottom() + bd);
    }

    return fr;
}

/**
 *  Aligns the center of \a aWidget with the center of \a aRelative.
 *
 *  If necessary, \a aWidget's position is adjusted to make it fully visible
 *  within the available desktop area. If \a aWidget is bigger then this area,
 *  it will also be resized unless \a aCanResize is false or there is an
 *  inappropriate minimum size limit (in which case the top left corner will be
 *  simply aligned with the top left corner of the available desktop area).
 *
 *  \a aWidget must be a top-level widget. \a aRelative may be any widget, but
 *  if it's not top-level itself, its top-level widget will be used for
 *  calculations. \a aRelative can also be NULL, in which case \a aWidget will
 *  be centered relative to the available desktop area.
 */
/* static */
void VBoxGlobal::centerWidget (QWidget *aWidget, QWidget *aRelative,
                               bool aCanResize /* = true */)
{
    AssertReturnVoid (aWidget);
    AssertReturnVoid (aWidget->isTopLevel());

    QRect deskGeo, parentGeo;
    QWidget *w = aRelative;
    if (w)
    {
        w = w->window();
        deskGeo = QApplication::desktop()->availableGeometry (w);
        parentGeo = w->frameGeometry();
        /* On X11/Gnome, geo/frameGeo.x() and y() are always 0 for top level
         * widgets with parents, what a shame. Use mapToGlobal() to workaround. */
        QPoint d = w->mapToGlobal (QPoint (0, 0));
        d.rx() -= w->geometry().x() - w->x();
        d.ry() -= w->geometry().y() - w->y();
        parentGeo.moveTopLeft (d);
    }
    else
    {
        deskGeo = QApplication::desktop()->availableGeometry();
        parentGeo = deskGeo;
    }

    /* On X11, there is no way to determine frame geometry (including WM
     * decorations) before the widget is shown for the first time. Stupidly
     * enumerate other top level widgets to find the thickest frame. The code
     * is based on the idea taken from QDialog::adjustPositionInternal(). */

    int extraw = 0, extrah = 0;

    QWidgetList list = QApplication::topLevelWidgets();
    QListIterator<QWidget*> it (list);
    while ((extraw == 0 || extrah == 0) && it.hasNext())
    {
        int framew, frameh;
        QWidget *current = it.next();
        if (!current->isVisible())
            continue;

        framew = current->frameGeometry().width() - current->width();
        frameh = current->frameGeometry().height() - current->height();

        extraw = QMAX (extraw, framew);
        extrah = QMAX (extrah, frameh);
    }

    /// @todo (r=dmik) not sure if we really need this
#if 0
    /* sanity check for decoration frames. With embedding, we
     * might get extraordinary values */
    if (extraw == 0 || extrah == 0 || extraw > 20 || extrah > 50)
    {
        extrah = 50;
        extraw = 20;
    }
#endif

    /* On non-X11 platforms, the following would be enough instead of the
     * above workaround: */
    // QRect geo = frameGeometry();
    QRect geo = QRect (0, 0, aWidget->width() + extraw,
                             aWidget->height() + extrah);

    geo.moveCenter (QPoint (parentGeo.x() + (parentGeo.width() - 1) / 2,
                            parentGeo.y() + (parentGeo.height() - 1) / 2));

    /* ensure the widget is within the available desktop area */
    QRect newGeo = normalizeGeometry (geo, deskGeo, aCanResize);

    aWidget->move (newGeo.topLeft());

    if (aCanResize &&
        (geo.width() != newGeo.width() || geo.height() != newGeo.height()))
        aWidget->resize (newGeo.width() - extraw, newGeo.height() - extrah);
}

/**
 *  Returns the decimal separator for the current locale.
 */
/* static */
QChar VBoxGlobal::decimalSep()
{
    return QLocale::system().decimalPoint();
}

/**
 *  Returns the regexp string that defines the format of the human-readable
 *  size representation, <tt>####[.##] B|KB|MB|GB|TB|PB</tt>.
 *
 *  This regexp will capture 5 groups of text:
 *  - cap(1): integer number in case when no decimal point is present
 *            (if empty, it means that decimal point is present)
 *  - cap(2): size suffix in case when no decimal point is present (may be empty)
 *  - cap(3): integer number in case when decimal point is present (may be empty)
 *  - cap(4): fraction number (hundredth) in case when decimal point is present
 *  - cap(5): size suffix in case when decimal point is present (note that
 *            B cannot appear there)
 */
/* static */
QString VBoxGlobal::sizeRegexp()
{
    QString regexp =
        QString ("^(?:(?:(\\d+)(?:\\s?([KMGTP]?B))?)|(?:(\\d*)%1(\\d{1,2})(?:\\s?([KMGTP]B))))$")
                 .arg (decimalSep());
    return regexp;
}

/**
 *  Parses the given size string that should be in form of
 *  <tt>####[.##] B|KB|MB|GB|TB|PB</tt> and returns the size value
 *  in bytes. Zero is returned on error.
 */
/* static */
Q_UINT64 VBoxGlobal::parseSize (const QString &aText)
{
    QRegExp regexp (sizeRegexp());
    int pos = regexp.indexIn (aText);
    if (pos != -1)
    {
        QString intgS = regexp.cap (1);
        QString hundS;
        QString suff = regexp.cap (2);
        if (intgS.isEmpty())
        {
            intgS = regexp.cap (3);
            hundS = regexp.cap (4);
            suff = regexp.cap (5);
        }

        Q_UINT64 denom = 0;
        if (suff.isEmpty() || suff == "B")
            denom = 1;
        else if (suff == "KB")
            denom = _1K;
        else if (suff == "MB")
            denom = _1M;
        else if (suff == "GB")
            denom = _1G;
        else if (suff == "TB")
            denom = _1T;
        else if (suff == "PB")
            denom = _1P;

        Q_UINT64 intg = intgS.toULongLong();
        if (denom == 1)
            return intg;

        Q_UINT64 hund = hundS.leftJustified (2, '0').toULongLong();
        hund = hund * denom / 100;
        intg = intg * denom + hund;
        return intg;
    }
    else
        return 0;
}

/**
 *  Formats the given \a size value in bytes to a human readable string
 *  in form of <tt>####[.##] B|KB|MB|GB|TB|PB</tt>.
 *
 *  The \a mode parameter is used for resulting numbers that get a fractional
 *  part after converting the \a size to KB, MB etc:
 *  <ul>
 *  <li>When \a mode is 0, the result is rounded to the closest number
 *      containing two decimal digits.
 *  </li>
 *  <li>When \a mode is -1, the result is rounded to the largest two decimal
 *      digit number that is not greater than the result. This guarantees that
 *      converting the resulting string back to the integer value in bytes
 *      will not produce a value greater that the initial \a size parameter.
 *  </li>
 *  <li>When \a mode is 1, the result is rounded to the smallest two decimal
 *      digit number that is not less than the result. This guarantees that
 *      converting the resulting string back to the integer value in bytes
 *      will not produce a value less that the initial \a size parameter.
 *  </li>
 *  </ul>
 *
 *  @param  aSize   size value in bytes
 *  @param  aMode   convertion mode (-1, 0 or 1)
 *  @return         human-readable size string
 */
/* static */
QString VBoxGlobal::formatSize (Q_UINT64 aSize, int aMode /* = 0 */)
{
    static const char *Suffixes [] = { "B", "KB", "MB", "GB", "TB", "PB", NULL };

    Q_UINT64 denom = 0;
    int suffix = 0;

    if (aSize < _1K)
    {
        denom = 1;
        suffix = 0;
    }
    else if (aSize < _1M)
    {
        denom = _1K;
        suffix = 1;
    }
    else if (aSize < _1G)
    {
        denom = _1M;
        suffix = 2;
    }
    else if (aSize < _1T)
    {
        denom = _1G;
        suffix = 3;
    }
    else if (aSize < _1P)
    {
        denom = _1T;
        suffix = 4;
    }
    else
    {
        denom = _1P;
        suffix = 5;
    }

    Q_UINT64 intg = aSize / denom;
    Q_UINT64 hund = aSize % denom;

    QString number;
    if (denom > 1)
    {
        if (hund)
        {
            hund *= 100;
            /* not greater */
            if (aMode < 0) hund = hund / denom;
            /* not less */
            else if (aMode > 0) hund = (hund + denom - 1) / denom;
            /* nearest */
            else hund = (hund + denom / 2) / denom;
        }
        /* check for the fractional part overflow due to rounding */
        if (hund == 100)
        {
            hund = 0;
            ++ intg;
            /* check if we've got 1024 XB after rounding and scale down if so */
            if (intg == 1024 && Suffixes [suffix + 1] != NULL)
            {
                intg /= 1024;
                ++ suffix;
            }
        }
        number = QString ("%1%2%3").arg (intg).arg (decimalSep())
                                   .arg (QString::number (hund).rightJustified (2, '0'));
    }
    else
    {
        number = QString::number (intg);
    }

    return QString ("%1 %2").arg (number).arg (Suffixes [suffix]);
}

/**
 *  Reformats the input string @a aStr so that:
 *  - strings in single quotes will be put inside <nobr> and marked
 *    with blue color;
 *  - UUIDs be put inside <nobr> and marked
 *    with green color;
 *  - replaces new line chars with </p><p> constructs to form paragraphs
 *    (note that <p> and </p> are not appended to the beginnign and to the
 *     end of the string respectively, to allow the result be appended
 *     or prepended to the existing paragraph).
 *
 *  If @a aToolTip is true, colouring is not applied, only the <nobr> tag
 *  is added. Also, new line chars are replaced with <br> instead of <p>.
 */
/* static */
QString VBoxGlobal::highlight (const QString &aStr, bool aToolTip /* = false */)
{
    QString strFont;
    QString uuidFont;
    QString endFont;
    if (!aToolTip)
    {
        strFont = "<font color=#0000CC>";
        uuidFont = "<font color=#008000>";
        endFont = "</font>";
    }

    QString text = aStr;

    /* replace special entities, '&' -- first! */
    text.replace ('&', "&amp;");
    text.replace ('<', "&lt;");
    text.replace ('>', "&gt;");
    text.replace ('\"', "&quot;");

    /* mark strings in single quotes with color */
    QRegExp rx = QRegExp ("((?:^|\\s)[(]?)'([^']*)'(?=[:.-!);]?(?:\\s|$))");
    rx.setMinimal (true);
    text.replace (rx,
        QString ("\\1%1<nobr>'\\2'</nobr>%2")
                 .arg (strFont). arg (endFont));

    /* mark UUIDs with color */
    text.replace (QRegExp (
        "((?:^|\\s)[(]?)"
        "(\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\})"
        "(?=[:.-!);]?(?:\\s|$))"),
        QString ("\\1%1<nobr>\\2</nobr>%2")
                 .arg (uuidFont). arg (endFont));

    /* split to paragraphs at \n chars */
    if (!aToolTip)
        text.replace ('\n', "</p><p>");
    else
        text.replace ('\n', "<br>");

    return text;
}

/**
 *  This does exactly the same as QLocale::system().name() but corrects its
 *  wrong behavior on Linux systems (LC_NUMERIC for some strange reason takes
 *  precedence over any other locale setting in the QLocale::system()
 *  implementation). This implementation first looks at LC_ALL (as defined by
 *  SUS), then looks at LC_MESSAGES which is designed to define a language for
 *  program messages in case if it differs from the language for other locale
 *  categories. Then it looks for LANG and finally falls back to
 *  QLocale::system().name().
 *
 *  The order of precedence is well defined here:
 *  http://opengroup.org/onlinepubs/007908799/xbd/envvar.html
 *
 *  @note This method will return "C" when the requested locale is invalid or
 *  when the "C" locale is set explicitly.
 */
/* static */
QString VBoxGlobal::systemLanguageId()
{
#ifdef Q_OS_UNIX
    const char *s = RTEnvGet ("LC_ALL");
    if (s == 0)
        s = RTEnvGet ("LC_MESSAGES");
    if (s == 0)
        s = RTEnvGet ("LANG");
    if (s != 0)
        return QLocale (s).name();
#endif
    return  QLocale::system().name();
}

/**
 *  Reimplementation of QFileDialog::getExistingDirectory() that removes some
 *  oddities and limitations.
 *
 *  On Win32, this function makes sure a native dialog is launched in
 *  another thread to avoid dialog visualization errors occuring due to
 *  multi-threaded COM apartment initialization on the main UI thread while
 *  the appropriate native dialog function expects a single-threaded one.
 *
 *  On all other platforms, this function is equivalent to
 *  QFileDialog::getExistingDirectory().
 */
QString VBoxGlobal::getExistingDirectory (const QString &aDir,
                                          QWidget *aParent,
                                          const QString &aCaption,
                                          bool aDirOnly,
                                          bool aResolveSymlinks)
{
#if defined Q_WS_WIN

    /**
     *  QEvent class reimplementation to carry Win32 API native dialog's
     *  result folder information
     */
    class GetExistDirectoryEvent : public OpenNativeDialogEvent
    {
    public:

        enum { TypeId = QEvent::User + 300 };

        GetExistDirectoryEvent (const QString &aResult)
            : OpenNativeDialogEvent (aResult, (QEvent::Type) TypeId) {}
    };

    /**
     *  QThread class reimplementation to open Win32 API native folder's dialog
     */
    class Thread : public QThread
    {
    public:

        Thread (QWidget *aParent, QObject *aTarget,
                const QString &aDir, const QString &aCaption)
            : mParent (aParent), mTarget (aTarget), mDir (aDir), mCaption (aCaption) {}

        virtual void run()
        {
            QString result;

            QWidget *topParent = mParent ? mParent->window() : vboxGlobal()->mainWindow();
            QString title = mCaption.isNull() ? tr ("Select a directory") : mCaption;

            TCHAR path[MAX_PATH];
            path [0] = 0;
            TCHAR initPath [MAX_PATH];
            initPath [0] = 0;

            BROWSEINFO bi;
            bi.hwndOwner = topParent ? topParent->winId() : 0;
            bi.pidlRoot = NULL;
            bi.lpszTitle = (TCHAR*)title.utf16();
            bi.pszDisplayName = initPath;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_STATUSTEXT | BIF_NEWDIALOGSTYLE;
            bi.lpfn = winGetExistDirCallbackProc;
            bi.lParam = Q_ULONG (&mDir);

            /* Qt is uncapable to properly handle modal state if the modal
             * window is not a QWidget. For example, if we have the W1->W2->N
             * ownership where Wx are QWidgets (W2 is modal), and N is a
             * native modal window, cliking on the title bar of W1 will still
             * activate W2 and redirect keyboard/mouse to it. The dirty hack
             * to prevent it is to disable the entire widget... */
            if (mParent)
                mParent->setEnabled (false);

            LPITEMIDLIST itemIdList = SHBrowseForFolder (&bi);
            if (itemIdList)
            {
                SHGetPathFromIDList (itemIdList, path);
                IMalloc *pMalloc;
                if (SHGetMalloc (&pMalloc) != NOERROR)
                    result = QString::null;
                else
                {
                    pMalloc->Free (itemIdList);
                    pMalloc->Release();
                    result = QString::fromUtf16 ((ushort*)path);
                }
            }
            else
                result = QString::null;
            QApplication::postEvent (mTarget, new GetExistDirectoryEvent (result));

            /* Enable the parent widget again. */
            if (mParent)
                mParent->setEnabled (true);
        }

    private:

        QWidget *mParent;
        QObject *mTarget;
        QString mDir;
        QString mCaption;
    };

    QString dir = QDir::convertSeparators (aDir);
    LoopObject loopObject ((QEvent::Type) GetExistDirectoryEvent::TypeId);
    Thread openDirThread (aParent, &loopObject, dir, aCaption);
    openDirThread.start();
    qApp->eventLoop()->enterLoop();
    openDirThread.wait();
    return loopObject.result();

#else

    QFileDialog::Options o;
    if (aDirOnly)
        o = QFileDialog::ShowDirsOnly;
    if (!aResolveSymlinks)
        o |= QFileDialog::DontResolveSymlinks;
    return QFileDialog::getExistingDirectory (aParent, aCaption, aDir, o);

#endif
}

/**
 *  Reimplementation of QFileDialog::getOpenFileName() that removes some
 *  oddities and limitations.
 *
 *  On Win32, this function makes sure a file filter is applied automatically
 *  right after it is selected from the drop-down list, to conform to common
 *  experience in other applications. Note that currently, @a selectedFilter
 *  is always set to null on return.
 *
 *  On all other platforms, this function is equivalent to
 *  QFileDialog::getOpenFileName().
 */
/* static */
QString VBoxGlobal::getOpenFileName (const QString &aStartWith,
                                     const QString &aFilters,
                                     QWidget       *aParent,
                                     const QString &aCaption,
                                     QString       *aSelectedFilter,
                                     bool           aResolveSymlinks)
{
#if defined Q_WS_WIN

    /**
     *  QEvent class reimplementation to carry Win32 API native dialog's
     *  result folder information
     */
    class GetOpenFileNameEvent : public OpenNativeDialogEvent
    {
    public:

        enum { TypeId = QEvent::User + 301 };

        GetOpenFileNameEvent (const QString &aResult)
            : OpenNativeDialogEvent (aResult, (QEvent::Type) TypeId) {}
    };

    /**
     *  QThread class reimplementation to open Win32 API native file dialog
     */
    class Thread : public QThread
    {
    public:

        Thread (QWidget *aParent, QObject *aTarget,
                const QString &aStartWith, const QString &aFilters,
                const QString &aCaption) :
                mParent (aParent), mTarget (aTarget),
                mStartWith (aStartWith), mFilters (aFilters),
                mCaption (aCaption) {}

        virtual void run()
        {
            QString result;

            QString workDir;
            QString initSel;
            QFileInfo fi (mStartWith);

            if (fi.isDir())
                workDir = mStartWith;
            else
            {
                workDir = fi.dirPath (true);
                initSel = fi.fileName();
            }

            workDir = QDir::convertSeparators (workDir);
            if (!workDir.endsWith ("\\"))
                workDir += "\\";

            QString title = mCaption.isNull() ? tr ("Select a file") : mCaption;

            QWidget *topParent = mParent ? mParent->window() : vboxGlobal()->mainWindow();
            QString winFilters = winFilter (mFilters);
            AssertCompile (sizeof (TCHAR) == sizeof (QChar));
            TCHAR buf [1024];
            if (initSel.length() > 0 && initSel.length() < sizeof (buf))
                memcpy (buf, initSel.utf16(), (initSel.length() + 1) * sizeof (TCHAR));
            else
                buf [0] = 0;

            OPENFILENAME ofn;
            memset (&ofn, 0, sizeof (OPENFILENAME));

            ofn.lStructSize = sizeof (OPENFILENAME);
            ofn.hwndOwner = topParent ? topParent->winId() : 0;
            ofn.lpstrFilter = (TCHAR *) winFilters.utf16();
            ofn.lpstrFile = buf;
            ofn.nMaxFile = sizeof (buf) - 1;
            ofn.lpstrInitialDir = (TCHAR *) workDir.utf16();
            ofn.lpstrTitle = (TCHAR *) title.utf16();
            ofn.Flags = (OFN_NOCHANGEDIR | OFN_HIDEREADONLY |
                          OFN_EXPLORER | OFN_ENABLEHOOK |
                          OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
            ofn.lpfnHook = OFNHookProc;

            if (GetOpenFileName (&ofn))
            {
                result = QString::fromUtf16 ((ushort *) ofn.lpstrFile);
            }

            // qt_win_eatMouseMove();
            MSG msg = {0, 0, 0, 0, 0, 0, 0};
            while (PeekMessage (&msg, 0, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE));
            if (msg.message == WM_MOUSEMOVE)
                PostMessage (msg.hwnd, msg.message, 0, msg.lParam);

            result = result.isEmpty() ? result : QFileInfo (result).absFilePath();

            QApplication::postEvent (mTarget, new GetOpenFileNameEvent (result));
        }

    private:

        QWidget *mParent;
        QObject *mTarget;
        QString mStartWith;
        QString mFilters;
        QString mCaption;
    };

    if (aSelectedFilter)
        *aSelectedFilter = QString::null;
    QString startWith = QDir::convertSeparators (aStartWith);
    LoopObject loopObject ((QEvent::Type) GetOpenFileNameEvent::TypeId);
    if (aParent) qt_enter_modal (aParent);
    Thread openDirThread (aParent, &loopObject, startWith, aFilters, aCaption);
    openDirThread.start();
    qApp->eventLoop()->enterLoop();
    openDirThread.wait();
    if (aParent) qt_leave_modal (aParent);
    return loopObject.result();

#else

    QFileDialog::Options o;
    if (!aResolveSymlinks)
        o |= QFileDialog::DontResolveSymlinks;
    return QFileDialog::getOpenFileName (aParent, aCaption, aStartWith,
                                         aFilters, aSelectedFilter, o);
#endif
}

/**
 *  Search for the first directory that exists starting from the passed one
 *  and going up through its parents.  In case if none of the directories
 *  exist (except the root one), the function returns QString::null.
 */
/* static */
QString VBoxGlobal::getFirstExistingDir (const QString &aStartDir)
{
    QString result = QString::null;
    QDir dir (aStartDir);
    while (!dir.exists() && !dir.isRoot())
    {
        QFileInfo dirInfo (dir.absolutePath());
        dir = dirInfo.absolutePath();
    }
    if (dir.exists() && !dir.isRoot())
        result = dir.absolutePath();
    return result;
}

#if defined (Q_WS_X11)

static char *XXGetProperty (Display *aDpy, Window aWnd,
                            Atom aPropType, const char *aPropName)
{
    Atom propNameAtom = XInternAtom (aDpy, aPropName,
                                     True /* only_if_exists */);
    if (propNameAtom == None)
        return NULL;

    Atom actTypeAtom = None;
    int actFmt = 0;
    unsigned long nItems = 0;
    unsigned long nBytesAfter = 0;
    unsigned char *propVal = NULL;
    int rc = XGetWindowProperty (aDpy, aWnd, propNameAtom,
                                 0, LONG_MAX, False /* delete */,
                                 aPropType, &actTypeAtom, &actFmt,
                                 &nItems, &nBytesAfter, &propVal);
    if (rc != Success)
        return NULL;

    return reinterpret_cast <char *> (propVal);
}

static Bool XXSendClientMessage (Display *aDpy, Window aWnd, const char *aMsg,
                                 unsigned long aData0 = 0, unsigned long aData1 = 0,
                                 unsigned long aData2 = 0, unsigned long aData3 = 0,
                                 unsigned long aData4 = 0)
{
    Atom msgAtom = XInternAtom (aDpy, aMsg, True /* only_if_exists */);
    if (msgAtom == None)
        return False;

    XEvent ev;

    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.display = aDpy;
    ev.xclient.window = aWnd;
    ev.xclient.message_type = msgAtom;

    /* always send as 32 bit for now */
    ev.xclient.format = 32;
    ev.xclient.data.l [0] = aData0;
    ev.xclient.data.l [1] = aData1;
    ev.xclient.data.l [2] = aData2;
    ev.xclient.data.l [3] = aData3;
    ev.xclient.data.l [4] = aData4;

    return XSendEvent (aDpy, DefaultRootWindow (aDpy), False,
                       SubstructureRedirectMask, &ev) != 0;
}

#endif

/**
 * Activates the specified window. If necessary, the window will be
 * de-iconified activation.
 *
 * @note On X11, it is implied that @a aWid represents a window of the same
 * display the application was started on.
 *
 * @param aWId              Window ID to activate.
 * @param aSwitchDesktop    @c true to switch to the window's desktop before
 *                          activation.
 *
 * @return @c true on success and @c false otherwise.
 */
/* static */
bool VBoxGlobal::activateWindow (WId aWId, bool aSwitchDesktop /* = true */)
{
    bool result = true;

#if defined (Q_WS_WIN32)

    if (IsIconic (aWId))
        result &= !!ShowWindow (aWId, SW_RESTORE);
    else if (!IsWindowVisible (aWId))
        result &= !!ShowWindow (aWId, SW_SHOW);

    result &= !!SetForegroundWindow (aWId);

#elif defined (Q_WS_X11)

    Display *dpy = QX11Info::display();

    if (aSwitchDesktop)
    {
        /* try to find the desktop ID using the NetWM property */
        CARD32 *desktop = (CARD32 *) XXGetProperty (dpy, aWId, XA_CARDINAL,
                                                    "_NET_WM_DESKTOP");
        if (desktop == NULL)
            /* if the NetWM propery is not supported try to find the desktop
             * ID using the GNOME WM property */
            desktop = (CARD32 *) XXGetProperty (dpy, aWId, XA_CARDINAL,
                                                "_WIN_WORKSPACE");

        if (desktop != NULL)
        {
            Bool ok = XXSendClientMessage (dpy, DefaultRootWindow (dpy),
                                           "_NET_CURRENT_DESKTOP",
                                           *desktop);
            if (!ok)
            {
                LogWarningFunc (("Couldn't switch to desktop=%08X\n",
                                 desktop));
                result = false;
            }
            XFree (desktop);
        }
        else
        {
            LogWarningFunc (("Couldn't find a desktop ID for aWId=%08X\n",
                             aWId));
            result = false;
        }
    }

    Bool ok = XXSendClientMessage (dpy, aWId, "_NET_ACTIVE_WINDOW");
    result &= !!ok;

    XRaiseWindow (dpy, aWId);

#else

    AssertFailed();
    result = false;

#endif

    if (!result)
        LogWarningFunc (("Couldn't activate aWId=%08X\n", aWId));

    return result;
}

/**
 *  Removes the acceletartor mark (the ampersand symbol) from the given string
 *  and returns the result. The string is supposed to be a menu item's text
 *  that may (or may not) contain the accelerator mark.
 *
 *  In order to support accelerators used in non-alphabet languages
 *  (e.g. Japanese) that has a form of "(&<L>)" (where <L> is a latin letter),
 *  this method first searches for this pattern and, if found, removes it as a
 *  whole. If such a pattern is not found, then the '&' character is simply
 *  removed from the string.
 *
 *  @note This function removes only the first occurense of the accelerator
 *  mark.
 *
 *  @param aText Menu item's text to remove the acceletaror mark from.
 *
 *  @return The resulting string.
 */
/* static */
QString VBoxGlobal::removeAccelMark (const QString &aText)
{
    QString result = aText;

    QRegExp accel ("\\(&[a-zA-Z]\\)");
    int pos = accel.indexIn (result);
    if (pos >= 0)
        result.remove (pos, accel.cap().length());
    else
    {
        pos = result.indexOf ('&');
        if (pos >= 0)
            result.remove (pos, 1);
    }

    return result;
}

/**
 *  Searches for a widget that with @a aName (if it is not NULL) which inherits
 *  @a aClassName (if it is not NULL) and among children of @a aParent. If @a
 *  aParent is NULL, all top-level widgets are searched. If @a aRecursive is
 *  true, child widgets are recursively searched as well.
 */
/* static */
QWidget *VBoxGlobal::findWidget (QWidget *aParent, const char *aName,
                                 const char *aClassName /* = NULL */,
                                 bool aRecursive /* = false */)
{
    if (aParent == NULL)
    {
        QWidgetList list = QApplication::topLevelWidgets();
        QWidget* w = NULL;
        foreach(w, list)
        {
            if ((!aName || strcmp (w->objectName().toAscii().constData(), aName) == 0) &&
                (!aClassName || strcmp (w->metaObject()->className(), aClassName) == 0))
                break;
            if (aRecursive)
            {
                w = findWidget (w, aName, aClassName, aRecursive);
                if (w)
                    break;
            }
        }
        return w;
    }

    /* Find the first children of aParent with the appropriate properties.
     * Please note that this call is recursivly. */
    QList<QWidget *> list = qFindChildren<QWidget *> (aParent, aName);
    QWidget *child = NULL;
    foreach(child, list)
    {
        if (!aClassName || strcmp (child->metaObject()->className(), aClassName) == 0)
            break;
    }
    return child;
}

// Public slots
////////////////////////////////////////////////////////////////////////////////

/**
 * Opens the specified URL using OS/Desktop capabilities.
 *
 * @param aURL URL to open
 *
 * @return true on success and false otherwise
 */
bool VBoxGlobal::openURL (const QString &aURL)
{
#if defined (Q_WS_WIN)
    /* We cannot use ShellExecute() on the main UI thread because we've
     * initialized COM with CoInitializeEx(COINIT_MULTITHREADED). See
     * http://support.microsoft.com/default.aspx?scid=kb;en-us;287087
     * for more details. */
    class Thread : public QThread
    {
    public:

        Thread (const QString &aURL, QObject *aObject)
            : mObject (aObject), mURL (aURL) {}

        void run()
        {
            int rc = (int) ShellExecute (NULL, NULL, mURL.utf16(), NULL, NULL, SW_SHOW);
            bool ok = rc > 32;
            QApplication::postEvent
                (mObject,
                 new VBoxShellExecuteEvent (this, mURL, ok));
        }

        QString mURL;
        QObject *mObject;
    };

    Thread *thread = new Thread (aURL, this);
    thread->start();
    /* thread will be deleted in the VBoxShellExecuteEvent handler */

    return true;

#elif defined (Q_WS_X11)

    static const char * const commands[] =
        { "kfmclient:exec", "gnome-open", "x-www-browser", "firefox", "konqueror" };

    for (size_t i = 0; i < ELEMENTS (commands); ++ i)
    {
        QStringList args = QString(commands [i]).split (':');
        args += aURL;
        QString command = args.takeFirst();
        if (QProcess::startDetached (command, args))
            return true;
    }

#elif defined (Q_WS_MAC)

    /* The code below is taken from Psi 0.10 sources
     * (http://www.psi-im.org) */

    /* Use Internet Config to hand the URL to the appropriate application, as
     * set by the user in the Internet Preferences pane.
     * NOTE: ICStart could be called once at Psi startup, saving the
     *       ICInstance in a global variable, as a minor optimization.
     *       ICStop should then be called at Psi shutdown if ICStart
     *       succeeded. */
    ICInstance icInstance;
    OSType psiSignature = 'psi ';
    OSStatus error = ::ICStart (&icInstance, psiSignature);
    if (error == noErr)
    {
        ConstStr255Param hint (0x0);
        QByteArray cs = aURL.toLocal8Bit();
        const char* data = cs.data();
        long length = cs.length();
        long start (0);
        long end (length);
        /* Don't bother testing return value (error); launched application
         * will report problems. */
        ::ICLaunchURL (icInstance, hint, data, length, &start, &end);
        ICStop (icInstance);
        return true;
    }

#else
    vboxProblem().message
        (NULL, VBoxProblemReporter::Error,
         tr ("Opening URLs is not implemented yet."));
    return false;
#endif

    /* if we go here it means we couldn't open the URL */
    vboxProblem().cannotOpenURL (aURL);

    return false;
}

void VBoxGlobal::showRegistrationDialog (bool aForce)
{
#ifdef VBOX_WITH_REGISTRATION
    if (!aForce && !VBoxRegistrationDlg::hasToBeShown())
        return;

    if (mRegDlg)
    {
        /* Show the already opened registration dialog */
        mRegDlg->setWindowState (mRegDlg->windowState() & ~Qt::WindowMinimized);
        mRegDlg->raise();
        mRegDlg->activateWindow();
    }
    else
    {
        /* Store the ID of the main window to ensure that only one
         * registration dialog is shown at a time. Due to manipulations with
         * OnExtraDataCanChange() and OnExtraDataChange() signals, this extra
         * data item acts like an inter-process mutex, so the first process
         * that attempts to set it will win, the rest will get a failure from
         * the SetExtraData() call. */
        mVBox.SetExtraData (VBoxDefs::GUI_RegistrationDlgWinID,
            QString ("%1").arg (static_cast<long> (mMainWindow->winId())));

        if (mVBox.isOk())
        {
            /* We've got the "mutex", create a new registration dialog */
            VBoxRegistrationDlg *dlg =
                new VBoxRegistrationDlg (&mRegDlg, 0, Qt::WDestructiveClose);
            Assert (dlg == mRegDlg);
            mRegDlg->show();
        }
    }
#endif
}

// Protected members
////////////////////////////////////////////////////////////////////////////////

bool VBoxGlobal::event (QEvent *e)
{
    switch (e->type())
    {
#if defined (Q_WS_WIN)
        case VBoxDefs::ShellExecuteEventType:
        {
            VBoxShellExecuteEvent *ev = (VBoxShellExecuteEvent *) e;
            if (!ev->mOk)
                vboxProblem().cannotOpenURL (ev->mURL);
            /* wait for the thread and free resources */
            ev->mThread->wait();
            delete ev->mThread;
            return true;
        }
#endif

        case VBoxDefs::AsyncEventType:
        {
            VBoxAsyncEvent *ev = (VBoxAsyncEvent *) e;
            ev->handle();
            return true;
        }

        case VBoxDefs::EnumerateMediaEventType:
        {
            VBoxEnumerateMediaEvent *ev = (VBoxEnumerateMediaEvent *) e;

            if (!ev->mLast)
            {
                if (ev->mMedia.status == VBoxMedia::Error)
                    vboxProblem().cannotGetMediaAccessibility (ev->mMedia.disk);
                media_list [ev->mIndex] = ev->mMedia;
                emit mediaEnumerated (media_list [ev->mIndex], ev->mIndex);
            }
            else
            {
                /* the thread has posted the last message, wait for termination */
                media_enum_thread->wait();
                delete media_enum_thread;
                media_enum_thread = 0;

                emit mediaEnumFinished (media_list);
            }

            return true;
        }

        /* VirtualBox callback events */

        case VBoxDefs::MachineStateChangeEventType:
        {
            emit machineStateChanged (*(VBoxMachineStateChangeEvent *) e);
            return true;
        }
        case VBoxDefs::MachineDataChangeEventType:
        {
            emit machineDataChanged (*(VBoxMachineDataChangeEvent *) e);
            return true;
        }
        case VBoxDefs::MachineRegisteredEventType:
        {
            emit machineRegistered (*(VBoxMachineRegisteredEvent *) e);
            return true;
        }
        case VBoxDefs::SessionStateChangeEventType:
        {
            emit sessionStateChanged (*(VBoxSessionStateChangeEvent *) e);
            return true;
        }
        case VBoxDefs::SnapshotEventType:
        {
            emit snapshotChanged (*(VBoxSnapshotEvent *) e);
            return true;
        }
        case VBoxDefs::CanShowRegDlgEventType:
        {
            emit canShowRegDlg (((VBoxCanShowRegDlgEvent *) e)->mCanShow);
            return true;
        }
        case VBoxDefs::ChangeGUILanguageEventType:
        {
            loadLanguage (static_cast<VBoxChangeGUILanguageEvent*> (e)->mLangId);
            return true;
        }

        default:
            break;
    }

    return QObject::event (e);
}

bool VBoxGlobal::eventFilter (QObject *aObject, QEvent *aEvent)
{
    if (aEvent->type() == QEvent::LanguageChange &&
        aObject->isWidgetType() &&
        static_cast <QWidget *> (aObject)->isTopLevel())
    {
        /* Catch the language change event before any other widget gets it in
         * order to invalidate cached string resources (like the details view
         * templates) that may be used by other widgets. */
        QWidgetList list = QApplication::topLevelWidgets();
        if (list.first() == aObject)
        {
            /* call this only once per every language change (see
             * QApplication::installTranslator() for details) */
            retranslateUi();
        }
    }

    return QObject::eventFilter (aObject, aEvent);
}

// Private members
////////////////////////////////////////////////////////////////////////////////

void VBoxGlobal::init()
{
#ifdef DEBUG
    verString += " [DEBUG]";
#endif

#ifdef Q_WS_WIN
    /* COM for the main thread is initialized in main() */
#else
    HRESULT rc = COMBase::InitializeCOM();
    if (FAILED (rc))
    {
        vboxProblem().cannotInitCOM (rc);
        return;
    }
#endif

    mVBox.createInstance (CLSID_VirtualBox);
    if (!mVBox.isOk())
    {
        vboxProblem().cannotCreateVirtualBox (mVBox);
        return;
    }

    /* initialize guest OS type vector */
    CGuestOSTypeCollection coll = mVBox.GetGuestOSTypes();
    int osTypeCount = coll.GetCount();
    AssertMsg (osTypeCount > 0, ("Number of OS types must not be zero"));
    if (osTypeCount > 0)
    {
        vm_os_types.resize (osTypeCount);
        int i = 0;
        CGuestOSTypeEnumerator en = coll.Enumerate();
        while (en.HasMore())
            vm_os_types [i++] = en.GetNext();
    }

    /* fill in OS type icon dictionary */
    static const char *kOSTypeIcons [][2] =
    {
        {"unknown",     ":/os_unknown.png"},
        {"dos",         ":/os_dos.png"},
        {"win31",       ":/os_win31.png"},
        {"win95",       ":/os_win95.png"},
        {"win98",       ":/os_win98.png"},
        {"winme",       ":/os_winme.png"},
        {"winnt4",      ":/os_winnt4.png"},
        {"win2k",       ":/os_win2k.png"},
        {"winxp",       ":/os_winxp.png"},
        {"win2k3",      ":/os_win2k3.png"},
        {"winvista",    ":/os_winvista.png"},
        {"win2k8",      ":/os_win2k8.png"},
        {"os2warp3",    ":/os_os2warp3.png"},
        {"os2warp4",    ":/os_os2warp4.png"},
        {"os2warp45",   ":/os_os2warp45.png"},
        {"ecs",         ":/os_ecs.png"},
        {"linux22",     ":/os_linux22.png"},
        {"linux24",     ":/os_linux24.png"},
        {"linux26",     ":/os_linux26.png"},
        {"archlinux",   ":/os_archlinux.png"},
        {"debian",      ":/os_debian.png"},
        {"opensolaris", ":/os_opensolaris.png"},
        {"opensuse",    ":/os_opensuse.png"},
        {"fedoracore",  ":/os_fedoracore.png"},
        {"gentoo",      ":/os_gentoo.png"},
        {"mandriva",    ":/os_mandriva.png"},
        {"redhat",      ":/os_redhat.png"},
        {"ubuntu",      ":/os_ubuntu.png"},
        {"xandros",     ":/os_xandros.png"},
        {"freebsd",     ":/os_freebsd.png"},
        {"openbsd",     ":/os_openbsd.png"},
        {"netbsd",      ":/os_netbsd.png"},
        {"netware",     ":/os_netware.png"},
        {"solaris",     ":/os_solaris.png"},
        {"l4",          ":/os_l4.png"},
    };
    for (uint n = 0; n < SIZEOF_ARRAY (kOSTypeIcons); n ++)
    {
        vm_os_type_icons.insert (kOSTypeIcons [n][0],
            new QPixmap (kOSTypeIcons [n][1]));
    }

    /* fill in VM state icon dictionary */
    static struct
    {
        KMachineState state;
        const char *name;
    }
    vmStateIcons[] =
    {
        {KMachineState_Null, NULL},
        {KMachineState_PoweredOff, ":/state_powered_off_16px.png"},
        {KMachineState_Saved, ":/state_saved_16px.png"},
        {KMachineState_Aborted, ":/state_aborted_16px.png"},
        {KMachineState_Running, ":/state_running_16px.png"},
        {KMachineState_Paused, ":/state_paused_16px.png"},
        {KMachineState_Stuck, ":/state_stuck_16px.png"},
        {KMachineState_Starting, ":/state_running_16px.png"}, /// @todo (dmik) separate icon?
        {KMachineState_Stopping, ":/state_running_16px.png"}, /// @todo (dmik) separate icon?
        {KMachineState_Saving, ":/state_saving_16px.png"},
        {KMachineState_Restoring, ":/state_restoring_16px.png"},
        {KMachineState_Discarding, ":/state_discarding_16px.png"},
    };
    for (uint n = 0; n < SIZEOF_ARRAY (vmStateIcons); n ++)
    {
        mStateIcons.insert (vmStateIcons [n].state,
            new QPixmap (vmStateIcons [n].name));
    }

    /* online/offline snapshot icons */
    mOfflineSnapshotIcon = QPixmap (":/offline_snapshot_16px.png");
    mOnlineSnapshotIcon = QPixmap (":/online_snapshot_16px.png");

    /* initialize state colors vector */
    vm_state_color.insert (KMachineState_Null,           new QColor(Qt::red));
    vm_state_color.insert (KMachineState_PoweredOff,     new QColor(Qt::gray));
    vm_state_color.insert (KMachineState_Saved,          new QColor(Qt::yellow));
    vm_state_color.insert (KMachineState_Aborted,        new QColor(Qt::darkRed));
    vm_state_color.insert (KMachineState_Running,        new QColor(Qt::green));
    vm_state_color.insert (KMachineState_Paused,         new QColor(Qt::darkGreen));
    vm_state_color.insert (KMachineState_Stuck,          new QColor(Qt::darkMagenta));
    vm_state_color.insert (KMachineState_Starting,       new QColor(Qt::green));
    vm_state_color.insert (KMachineState_Stopping,       new QColor(Qt::green));
    vm_state_color.insert (KMachineState_Saving,         new QColor(Qt::green));
    vm_state_color.insert (KMachineState_Restoring,      new QColor(Qt::green));
    vm_state_color.insert (KMachineState_Discarding,     new QColor(Qt::green));

    qApp->installEventFilter (this);

    /* create default non-null global settings */
    gset = VBoxGlobalSettings (false);

    /* try to load global settings */
    gset.load (mVBox);
    if (!mVBox.isOk() || !gset)
    {
        vboxProblem().cannotLoadGlobalConfig (mVBox, gset.lastError());
        return;
    }

    /* Load customized language if any */
    QString languageId = gset.languageId();
    if (!languageId.isNull())
        loadLanguage (languageId);

    retranslateUi();

    /* process command line */

    vm_render_mode_str = 0;
#ifdef VBOX_WITH_DEBUGGER_GUI
#ifdef VBOX_WITH_DEBUGGER_GUI_MENU
    dbg_enabled = true;
#else
    dbg_enabled = false;
#endif
    dbg_visible_at_startup = false;
#endif

    int argc = qApp->argc();
    int i = 1;
    while (i < argc)
    {
        const char *arg = qApp->argv() [i];
        if (       !::strcmp (arg, "-startvm"))
        {
            if (++i < argc)
            {
                QString param = QString (qApp->argv() [i]);
                QUuid uuid = QUuid (param);
                if (!uuid.isNull())
                {
                    vmUuid = uuid;
                }
                else
                {
                    CMachine m = mVBox.FindMachine (param);
                    if (m.isNull())
                    {
                        vboxProblem().cannotFindMachineByName (mVBox, param);
                        return;
                    }
                    vmUuid = m.GetId();
                }
            }
        }
        else if (!::strcmp (arg, "-comment"))
        {
            ++i;
        }
        else if (!::strcmp (arg, "-rmode"))
        {
            if (++i < argc)
                vm_render_mode_str = qApp->argv() [i];
        }
#ifdef VBOX_WITH_DEBUGGER_GUI
        else if (!::strcmp (arg, "-dbg"))
        {
            dbg_enabled = true;
        }
#ifdef DEBUG
        else if (!::strcmp (arg, "-nodebug"))
        {
            dbg_enabled = false;
            dbg_visible_at_startup = false;
        }
#else
        else if (!::strcmp( arg, "-debug"))
        {
            dbg_enabled = true;
            dbg_visible_at_startup = true;
        }
#endif
#endif
        i++;
    }

    vm_render_mode = vboxGetRenderMode( vm_render_mode_str );

    /* setup the callback */
    callback = CVirtualBoxCallback (new VBoxCallback (*this));
    mVBox.RegisterCallback (callback);
    AssertWrapperOk (mVBox);
    if (!mVBox.isOk())
        return;

    mValid = true;
}

/** @internal
 *
 *  This method should be never called directly. It is called automatically
 *  when the application terminates.
 */
void VBoxGlobal::cleanup()
{
    /* sanity check */
    if (!sVBoxGlobalInCleanup)
    {
        AssertMsgFailed (("Should never be called directly\n"));
        return;
    }

    if (!callback.isNull())
    {
        mVBox.UnregisterCallback (callback);
        AssertWrapperOk (mVBox);
        callback.detach();
    }

    if (media_enum_thread)
    {
        /* sVBoxGlobalInCleanup is true here, so just wait for the thread */
        media_enum_thread->wait();
        delete media_enum_thread;
        media_enum_thread = 0;
    }

#ifdef VBOX_WITH_REGISTRATION
    if (mRegDlg)
        mRegDlg->close();
#endif

    if (mConsoleWnd)
        delete mConsoleWnd;
    if (mSelectorWnd)
        delete mSelectorWnd;

    /* ensure CGuestOSType objects are no longer used */
    vm_os_types.clear();
    /* media list contains a lot of CUUnknown, release them */
    media_list.clear();
    /* the last step to ensure we don't use COM any more */
    mVBox.detach();

    /* There may be VBoxEnumerateMediaEvent instances still in the message
     * queue which reference COM objects. Remove them to release those objects
     * before uninitializing the COM subsystem. */
    QApplication::removePostedEvents (this);

#ifdef Q_WS_WIN
    /* COM for the main thread is shutdown in main() */
#else
    COMBase::CleanupCOM();
#endif

    mValid = false;
}

/** @fn vboxGlobal
 *
 *  Shortcut to the static VBoxGlobal::instance() method, for convenience.
 */


/**
 *  USB Popup Menu class methods
 *  This class provides the list of USB devices attached to the host.
 */
VBoxUSBMenu::VBoxUSBMenu (QWidget *aParent) : QMenu (aParent)
{
    connect (this, SIGNAL (aboutToShow()),
             this, SLOT   (processAboutToShow()));
//    connect (this, SIGNAL (hovered (QAction *)),
//             this, SLOT   (processHighlighted (QAction *)));
}

const CUSBDevice& VBoxUSBMenu::getUSB (QAction *aAction)
{
    return mUSBDevicesMap [aAction];
}

void VBoxUSBMenu::setConsole (const CConsole &aConsole)
{
    mConsole = aConsole;
}

void VBoxUSBMenu::processAboutToShow()
{
    clear();
    mUSBDevicesMap.clear();

    CHost host = vboxGlobal().virtualBox().GetHost();

    bool isUSBEmpty = host.GetUSBDevices().GetCount() == 0;
    if (isUSBEmpty)
    {
        QAction *action = addAction (tr ("<no available devices>", "USB devices"));
        action->setEnabled (false);
        action->setToolTip (tr ("No supported devices connected to the host PC",
                                "USB device tooltip"));
    }
    else
    {
        CHostUSBDeviceEnumerator en = host.GetUSBDevices().Enumerate();
        while (en.HasMore())
        {
            CHostUSBDevice iterator = en.GetNext();
            CUSBDevice usb = CUnknown (iterator);
            QAction *action = addAction (vboxGlobal().details (usb));
            action->setCheckable (true);
            mUSBDevicesMap [action] = usb;
            /* check if created item was alread attached to this session */
            if (!mConsole.isNull())
            {
                CUSBDevice attachedUSB =
                    mConsole.GetUSBDevices().FindById (usb.GetId());
                action->setChecked (!attachedUSB.isNull());
                action->setEnabled (iterator.GetState() !=
                                    KUSBDeviceState_Unavailable);
            }
        }
    }
}

bool VBoxUSBMenu::event(QEvent *aEvent)
{
    /* We provide dynamic tooltips for the usb devices */
    if (aEvent->type() == QEvent::ToolTip)
    {
        QHelpEvent *helpEvent = static_cast<QHelpEvent *> (aEvent);
        QAction *action = actionAt (helpEvent->pos());
        if (action)
        {
            CUSBDevice usb = mUSBDevicesMap [action];
            if (!usb.isNull())
            {
                QToolTip::showText (helpEvent->globalPos(), vboxGlobal().toolTip (usb));
                return true;
            }
        }
    }
    return QMenu::event (aEvent);
}

/**
 *  Enable/Disable Menu class.
 *  This class provides enable/disable menu items.
 */
VBoxSwitchMenu::VBoxSwitchMenu (QWidget *aParent, QAction *aAction,
                                bool aInverted)
    : QMenu (aParent), mAction (aAction), mInverted (aInverted)
{
    /* this menu works only with toggle action */
    Assert (aAction->isCheckable());
    addAction(aAction);
    connect (this, SIGNAL (aboutToShow()),
             this, SLOT   (processAboutToShow()));
}

void VBoxSwitchMenu::setToolTip (const QString &aTip)
{
    mAction->setToolTip (aTip);
}

void VBoxSwitchMenu::processAboutToShow()
{
    QString text = mAction->isChecked() ^ mInverted ? tr ("Disable") : tr ("Enable");
    mAction->setText (text);
}

#ifdef Q_WS_X11
#include "VBoxGlobal.moc"
#endif
