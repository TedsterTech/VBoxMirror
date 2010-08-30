/* $Id$ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UISession stuff implementation
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Global inclues */
#include <QApplication>
#include <QWidget>
#include <QTimer>

/* Local includes */
#include "UISession.h"
#include "UIMachine.h"
#include "UIActionsPool.h"
#include "UIMachineLogic.h"
#include "UIMachineWindow.h"
#include "UIMachineMenuBar.h"
#include "VBoxProblemReporter.h"
#include "UIFirstRunWzd.h"
#include "UIConsoleEventHandler.h"
#ifdef VBOX_WITH_VIDEOHWACCEL
# include "VBoxFBOverlay.h"
# include "UIFrameBuffer.h"
#endif

#ifdef Q_WS_X11
# include <QX11Info>
# include <X11/Xlib.h>
# include <X11/Xutil.h>
# ifndef VBOX_WITHOUT_XCURSOR
#  include <X11/Xcursor/Xcursor.h>
# endif
#endif

UISession::UISession(UIMachine *pMachine, CSession &sessionReference)
    : QObject(pMachine)
    /* Base variables: */
    , m_pMachine(pMachine)
    , m_session(sessionReference)
    /* Common variables: */
    , m_pMenuPool(0)
#ifdef VBOX_WITH_VIDEOHWACCEL
    , m_FrameBufferVector(sessionReference.GetMachine().GetMonitorCount())
#endif
    , m_machineState(KMachineState_Null)
#if defined(Q_WS_WIN)
    , m_alphaCursor(0)
#endif
    /* Common flags: */
    , m_fIsFirstTimeStarted(false)
    , m_fIsIgnoreRuntimeMediumsChanging(false)
    , m_fIsGuestResizeIgnored(false)
    , m_fIsSeamlessModeRequested(false)
    , m_fIsAutoCaptureDisabled(false)
    /* Guest additions flags: */
    , m_ulGuestAdditionsRunLevel(0)
    , m_fIsGuestSupportsGraphics(false)
    , m_fIsGuestSupportsSeamless(false)
    /* Mouse flags: */
    , m_fNumLock(false)
    , m_fCapsLock(false)
    , m_fScrollLock(false)
    , m_uNumLockAdaptionCnt(2)
    , m_uCapsLockAdaptionCnt(2)
    /* Mouse flags: */
    , m_fIsMouseSupportsAbsolute(false)
    , m_fIsMouseSupportsRelative(false)
    , m_fIsMouseHostCursorNeeded(false)
    , m_fIsMouseCaptured(false)
    , m_fIsMouseIntegrated(true)
    , m_fIsValidPointerShapePresent(false)
    , m_fIsHidingHostPointer(true)
{
    /* Explicit initialize the console event handler */
    UIConsoleEventHandler::instance(this);

    /* Add console event connections */
    connect(gConsoleEvents, SIGNAL(sigMousePointerShapeChange(bool, bool, QPoint, QSize, QVector<uint8_t>)),
            this, SLOT(sltMousePointerShapeChange(bool, bool, QPoint, QSize, QVector<uint8_t>)));

    connect(gConsoleEvents, SIGNAL(sigMouseCapabilityChange(bool, bool, bool)),
            this, SLOT(sltMouseCapabilityChange(bool, bool, bool)));

    connect(gConsoleEvents, SIGNAL(sigKeyboardLedsChangeEvent(bool, bool, bool)),
            this, SLOT(sltKeyboardLedsChangeEvent(bool, bool, bool)));

    connect(gConsoleEvents, SIGNAL(sigStateChange(KMachineState)),
            this, SLOT(sltStateChange(KMachineState)));

    connect(gConsoleEvents, SIGNAL(sigAdditionsChange()),
            this, SLOT(sltAdditionsChange()));

    connect(gConsoleEvents, SIGNAL(sigNetworkAdapterChange(CNetworkAdapter)),
            this, SIGNAL(sigNetworkAdapterChange(CNetworkAdapter)));

    connect(gConsoleEvents, SIGNAL(sigMediumChange(CMediumAttachment)),
            this, SIGNAL(sigMediumChange(CMediumAttachment)));

    connect(gConsoleEvents, SIGNAL(sigUSBControllerChange()),
            this, SIGNAL(sigUSBControllerChange()));

    connect(gConsoleEvents, SIGNAL(sigUSBDeviceStateChange(CUSBDevice, bool, CVirtualBoxErrorInfo)),
            this, SIGNAL(sigUSBDeviceStateChange(CUSBDevice, bool, CVirtualBoxErrorInfo)));

    connect(gConsoleEvents, SIGNAL(sigSharedFolderChange()),
            this, SIGNAL(sigSharedFolderChange()));

    connect(gConsoleEvents, SIGNAL(sigRuntimeError(bool, QString, QString)),
            this, SIGNAL(sigRuntimeError(bool, QString, QString)));

#ifdef Q_WS_MAC
    connect(gConsoleEvents, SIGNAL(sigShowWindow()),
            this, SIGNAL(sigShowWindows()),
            Qt::QueuedConnection);
#endif /* Q_WS_MAC */

    /* Prepare main menu: */
    prepareMenuPool();

    /* Load uisession settings: */
    loadSessionSettings();
}

UISession::~UISession()
{
    /* Save uisession settings: */
    saveSessionSettings();

    /* Cleanup main menu: */
    cleanupMenuPool();

    /* Destroy the console event handler */
    UIConsoleEventHandler::destroy();

#if defined(Q_WS_WIN)
    /* Destroy alpha cursor: */
    if (m_alphaCursor)
        DestroyIcon(m_alphaCursor);
#endif

#ifdef VBOX_WITH_VIDEOHWACCEL
    for (int i = m_FrameBufferVector.size() - 1; i >= 0; --i)
    {
        UIFrameBuffer *pFb = m_FrameBufferVector[i];
        if (pFb)
        {
            /* Warn framebuffer about its no more necessary: */
            pFb->setDeleted(true);
            /* Detach framebuffer from Display: */
            CDisplay display = session().GetConsole().GetDisplay();
            display.SetFramebuffer(i, CFramebuffer(NULL));
            /* Release the reference: */
            pFb->Release();
        }
    }
#endif
}

void UISession::powerUp()
{
    /* Do nothing if we had started already: */
    if (isRunning() || isPaused())
        return;

    /* Prepare powerup: */
    preparePowerUp();

    /* Get current machine/console: */
    CMachine machine = session().GetMachine();
    CConsole console = session().GetConsole();

    /* Power UP machine: */
    CProgress progress = vboxGlobal().isStartPausedEnabled() || vboxGlobal().isDebuggerAutoShowEnabled() ?
                         console.PowerUpPaused() : console.PowerUp();

    /* Check for immediate failure: */
    if (!console.isOk())
    {
        if (vboxGlobal().showStartVMErrors())
            vboxProblem().cannotStartMachine(console);
        QTimer::singleShot(0, this, SLOT(sltCloseVirtualSession()));
        return;
    }

    /* Guard progressbar warnings from auto-closing: */
    if (uimachine()->machineLogic())
        uimachine()->machineLogic()->setPreventAutoClose(true);

    /* Show "Starting/Restoring" progress dialog: */
    if (isSaved())
        vboxProblem().showModalProgressDialog(progress, machine.GetName(), mainMachineWindow(), 0);
    else
        vboxProblem().showModalProgressDialog(progress, machine.GetName(), mainMachineWindow());

    /* Allow further auto-closing: */
    if (uimachine()->machineLogic())
        uimachine()->machineLogic()->setPreventAutoClose(false);

    /* Check for a progress failure: */
    if (progress.GetResultCode() != 0)
    {
        if (vboxGlobal().showStartVMErrors())
            vboxProblem().cannotStartMachine(progress);
        QTimer::singleShot(0, this, SLOT(sltCloseVirtualSession()));
        return;
    }

    /* Check if we missed a really quick termination after successful startup, and process it if we did: */
    if (isTurnedOff())
    {
        QTimer::singleShot(0, this, SLOT(sltCloseVirtualSession()));
        return;
    }

    /* Check if the required virtualization features are active. We get this
     * info only when the session is active. */
    bool fIs64BitsGuest = vboxGlobal().virtualBox().GetGuestOSType(console.GetGuest().GetOSTypeId()).GetIs64Bit();
    bool fRecommendVirtEx = vboxGlobal().virtualBox().GetGuestOSType(console.GetGuest().GetOSTypeId()).GetRecommendedVirtEx();
    AssertMsg(!fIs64BitsGuest || fRecommendVirtEx, ("Virtualization support missed for 64bit guest!\n"));
    bool fIsVirtEnabled = console.GetDebugger().GetHWVirtExEnabled();
    if (fRecommendVirtEx && !fIsVirtEnabled)
    {
        bool fShouldWeClose;

        bool fVTxAMDVSupported = vboxGlobal().virtualBox().GetHost().GetProcessorFeature(KProcessorFeature_HWVirtEx);

        QApplication::processEvents();
        setPause(true);

        if (fIs64BitsGuest)
            fShouldWeClose = vboxProblem().warnAboutVirtNotEnabled64BitsGuest(fVTxAMDVSupported);
        else
            fShouldWeClose = vboxProblem().warnAboutVirtNotEnabledGuestRequired(fVTxAMDVSupported);

        if (fShouldWeClose)
        {
            /* At this point the console is powered up. So we have to close
             * this session again. */
            CProgress progress = console.PowerDown();
            if (console.isOk())
            {
                /* Guard progressbar warnings from auto-closing: */
                if (uimachine()->machineLogic())
                    uimachine()->machineLogic()->setPreventAutoClose(true);
                /* Show the power down progress dialog */
                vboxProblem().showModalProgressDialog(progress, machine.GetName(), mainMachineWindow());
                if (progress.GetResultCode() != 0)
                    vboxProblem().cannotStopMachine(progress);
                /* Allow further auto-closing: */
                if (uimachine()->machineLogic())
                    uimachine()->machineLogic()->setPreventAutoClose(false);
            }
            else
                vboxProblem().cannotStopMachine(console);
            /* Now signal the destruction of the rest. */
            QTimer::singleShot(0, this, SLOT(sltCloseVirtualSession()));
            return;
        }
        else
            setPause(false);
    }

#ifdef VBOX_WITH_VIDEOHWACCEL
    LogRel(("2D video acceleration is %s.\n",
           machine.GetAccelerate2DVideoEnabled() && VBoxGlobal::isAcceleration2DVideoAvailable()
                 ? "enabled"
                 : "disabled"));
#endif

    /* Warn listeners about machine was started: */
    emit sigMachineStarted();
}

UIActionsPool* UISession::actionsPool() const
{
    return m_pMachine->actionsPool();
}

QWidget* UISession::mainMachineWindow() const
{
    return uimachine()->machineLogic()->mainMachineWindow()->machineWindow();
}

UIMachineLogic* UISession::machineLogic() const
{
    return uimachine()->machineLogic();
}

QMenu* UISession::newMenu(UIMainMenuType fOptions /* = UIMainMenuType_ALL */)
{
    /* Create new menu: */
    QMenu *pMenu = m_pMenuPool->createMenu(actionsPool(), fOptions);

    /* Re-init menu pool for the case menu were recreated: */
    reinitMenuPool();

    /* Return newly created menu: */
    return pMenu;
}

QMenuBar* UISession::newMenuBar(UIMainMenuType fOptions /* = UIMainMenuType_ALL */)
{
    /* Create new menubar: */
    QMenuBar *pMenuBar = m_pMenuPool->createMenuBar(actionsPool(), fOptions);

    /* Re-init menu pool for the case menu were recreated: */
    reinitMenuPool();

    /* Return newly created menubar: */
    return pMenuBar;
}

bool UISession::setPause(bool fOn)
{
    /* Commenting it out as isPaused() could reflect
     * quite obsolete state due to synchronization: */
    //if (isPaused() == fOn)
    //    return true;

    CConsole console = session().GetConsole();

    if (fOn)
        console.Pause();
    else
        console.Resume();

    bool ok = console.isOk();
    if (!ok)
    {
        if (fOn)
            vboxProblem().cannotPauseMachine(console);
        else
            vboxProblem().cannotResumeMachine(console);
    }

    return ok;
}

void UISession::sltInstallGuestAdditionsFrom(const QString &strSource)
{
    CMachine machine = session().GetMachine();
    CVirtualBox vbox = vboxGlobal().virtualBox();
    QString strUuid;

    CMedium image = vbox.FindMedium(strSource, KDeviceType_DVD);
    if (image.isNull())
    {
        image = vbox.OpenMedium(strSource, KDeviceType_DVD, KAccessMode_ReadWrite);
        if (vbox.isOk())
            strUuid = image.GetId();
    }
    else
        strUuid = image.GetId();

    if (!vbox.isOk())
    {
        vboxProblem().cannotOpenMedium(0, vbox, VBoxDefs::MediumType_DVD, strSource);
        return;
    }

    AssertMsg(!strUuid.isNull(), ("Guest Additions image UUID should be valid!\n"));

    QString strCntName;
    LONG iCntPort = -1, iCntDevice = -1;
    /* Searching for the first suitable slot */
    {
        CStorageControllerVector controllers = machine.GetStorageControllers();
        int i = 0;
        while (i < controllers.size() && strCntName.isNull())
        {
            CStorageController controller = controllers[i];
            CMediumAttachmentVector attachments = machine.GetMediumAttachmentsOfController(controller.GetName());
            int j = 0;
            while (j < attachments.size() && strCntName.isNull())
            {
                CMediumAttachment attachment = attachments[j];
                if (attachment.GetType() == KDeviceType_DVD)
                {
                    strCntName = controller.GetName();
                    iCntPort = attachment.GetPort();
                    iCntDevice = attachment.GetDevice();
                }
                ++ j;
            }
            ++ i;
        }
    }

    if (!strCntName.isNull())
    {
        bool fIsMounted = false;

        /* Mount medium to the predefined port/device */
        machine.MountMedium(strCntName, iCntPort, iCntDevice, strUuid, false /* force */);
        if (machine.isOk())
            fIsMounted = true;
        else
        {
            /* Ask for force mounting */
            if (vboxProblem().cannotRemountMedium(0, machine, VBoxMedium(image, VBoxDefs::MediumType_DVD),
                                                  true /* mount? */, true /* retry? */) == QIMessageBox::Ok)
            {
                /* Force mount medium to the predefined port/device */
                machine.MountMedium(strCntName, iCntPort, iCntDevice, strUuid, true /* force */);
                if (machine.isOk())
                    fIsMounted = true;
                else
                    vboxProblem().cannotRemountMedium(0, machine, VBoxMedium(image, VBoxDefs::MediumType_DVD),
                                                      true /* mount? */, false /* retry? */);
            }
        }
    }
    else
        vboxProblem().cannotMountGuestAdditions(machine.GetName());
}

void UISession::sltCloseVirtualSession()
{
     /* Use a single shot with timeout 0 to allow the widgets to cleanly close and test then again.
      * If all open widgets are closed destroy ourself: */
    QWidget *widget = QApplication::activeModalWidget() ?
                      QApplication::activeModalWidget() :
                      QApplication::activePopupWidget() ?
                      QApplication::activePopupWidget() : 0;
    if (widget)
    {
        widget->hide();
        QTimer::singleShot(0, this, SLOT(sltCloseVirtualSession()));
        return;
    }
    m_pMachine->closeVirtualMachine();
}

void UISession::sltMousePointerShapeChange(bool fVisible, bool fAlpha, QPoint hotCorner, QSize size, QVector<uint8_t> shape)
{
    /* In case of shape data is present: */
    if (shape.size() > 0)
    {
        /* We are ignoring visibility flag: */
        m_fIsHidingHostPointer = false;

        /* And updating current cursor shape: */
        setPointerShape(shape.data(), fAlpha,
                        hotCorner.x(), hotCorner.y(),
                        size.width(), size.height());
    }
    /* In case of shape data is NOT present: */
    else
    {
        /* Remember if we should hide the cursor: */
        m_fIsHidingHostPointer = !fVisible;
    }

    /* Notify listeners about mouse capability changed: */
    emit sigMousePointerShapeChange();

}

void UISession::sltMouseCapabilityChange(bool fSupportsAbsolute, bool fSupportsRelative, bool fNeedsHostCursor)
{
    /* Check if something had changed: */
    if (   m_fIsMouseSupportsAbsolute != fSupportsAbsolute
        || m_fIsMouseSupportsRelative != fSupportsRelative
        || m_fIsMouseHostCursorNeeded != fNeedsHostCursor)
    {
        /* Store new data: */
        m_fIsMouseSupportsAbsolute = fSupportsAbsolute;
        m_fIsMouseSupportsRelative = fSupportsRelative;
        m_fIsMouseHostCursorNeeded = fNeedsHostCursor;

        /* Notify listeners about mouse capability changed: */
        emit sigMouseCapabilityChange();
    }
}

void UISession::sltKeyboardLedsChangeEvent(bool fNumLock, bool fCapsLock, bool fScrollLock)
{
    /* Check if something had changed: */
    if (   m_fNumLock != fNumLock
        || m_fCapsLock != fCapsLock
        || m_fScrollLock != fScrollLock)
    {
        /* Store new num lock data: */
        if (m_fNumLock != fNumLock)
        {
            m_fNumLock = fNumLock;
            m_uNumLockAdaptionCnt = 2;
        }

        /* Store new caps lock data: */
        if (m_fCapsLock != fCapsLock)
        {
            m_fCapsLock = fCapsLock;
            m_uCapsLockAdaptionCnt = 2;
        }

        /* Store new scroll lock data: */
        if (m_fScrollLock != fScrollLock)
        {
            m_fScrollLock = fScrollLock;
        }

        /* Notify listeners about mouse capability changed: */
        emit sigKeyboardLedsChange();
    }
}

void UISession::sltStateChange(KMachineState state)
{
    /* Check if something had changed: */
    if (m_machineState != state)
    {
        /* Store new data: */
        m_machineState = state;

        /* Notify listeners about machine state changed: */
        emit sigMachineStateChange();
    }
}

void UISession::sltAdditionsChange()
{
    /* Get our guest: */
    CGuest guest = session().GetConsole().GetGuest();

    /* Variable flags: */
    ULONG ulGuestAdditionsRunLevel = guest.GetAdditionsRunLevel();
    bool fIsGuestSupportsGraphics = guest.GetSupportsGraphics();
    bool fIsGuestSupportsSeamless = guest.GetSupportsSeamless();

    /* Check if something had changed: */
    if (m_ulGuestAdditionsRunLevel != ulGuestAdditionsRunLevel ||
        m_fIsGuestSupportsGraphics != fIsGuestSupportsGraphics ||
        m_fIsGuestSupportsSeamless != fIsGuestSupportsSeamless)
    {
        /* Store new data: */
        m_ulGuestAdditionsRunLevel = ulGuestAdditionsRunLevel;
        m_fIsGuestSupportsGraphics = fIsGuestSupportsGraphics;
        m_fIsGuestSupportsSeamless = fIsGuestSupportsSeamless;

        /* Notify listeners about guest additions state changed: */
        emit sigAdditionsStateChange();
    }
}

void UISession::prepareMenuPool()
{
    m_pMenuPool = new UIMachineMenuBar;
}

void UISession::loadSessionSettings()
{
   /* Get uisession machine: */
    CMachine machine = session().GetConsole().GetMachine();

    /* Availability settings: */
    {
        /* VRDP Stuff: */
        CVRDPServer vrdpServer = machine.GetVRDPServer();
        if (vrdpServer.isNull())
        {
            /* Hide VRDP Action: */
            uimachine()->actionsPool()->action(UIActionIndex_Toggle_VRDP)->setVisible(false);
        }
        else
        {
            /* Check/Uncheck VRDP action depending on VRDP server status: */
            uimachine()->actionsPool()->action(UIActionIndex_Toggle_VRDP)->setChecked(vrdpServer.GetEnabled());
        }
    }

    /* Load extra-data settings: */
    {
        /* Temporary: */
        QString strSettings;

        /* Is there shoul be First RUN Wizard? */
        strSettings = machine.GetExtraData(VBoxDefs::GUI_FirstRun);
        if (strSettings == "yes")
            m_fIsFirstTimeStarted = true;

        /* Ignore mediums mounted at runtime? */
        strSettings = machine.GetExtraData(VBoxDefs::GUI_SaveMountedAtRuntime);
        if (strSettings == "no")
            m_fIsIgnoreRuntimeMediumsChanging = true;

        /* Should guest autoresize? */
        strSettings = machine.GetExtraData(VBoxDefs::GUI_AutoresizeGuest);
        QAction *pGuestAutoresizeSwitch = uimachine()->actionsPool()->action(UIActionIndex_Toggle_GuestAutoresize);
        pGuestAutoresizeSwitch->setChecked(strSettings != "off");

#ifdef Q_WS_WIN
        /* Disable host screen-saver if requested: */
        if (vboxGlobal().settings().hostScreenSaverDisabled())
            SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, false, 0, 0);
#endif /* Q_WS_WIN */
    }
}

void UISession::saveSessionSettings()
{
    /* Get uisession machine: */
    CMachine machine = session().GetConsole().GetMachine();

    /* Save extra-data settings: */
    {
        /* Disable First RUN Wizard for the since now: */
        machine.SetExtraData(VBoxDefs::GUI_FirstRun, QString());

        /* Remember if guest should autoresize: */
        machine.SetExtraData(VBoxDefs::GUI_AutoresizeGuest,
                             uimachine()->actionsPool()->action(UIActionIndex_Toggle_GuestAutoresize)->isChecked() ?
                             QString() : "off");

#ifdef Q_WS_WIN
        /* Restore screen-saver activity to system default: */
        SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, true, 0, 0);
#endif /* Q_WS_WIN */
    }
}

void UISession::cleanupMenuPool()
{
    delete m_pMenuPool;
    m_pMenuPool = 0;
}

WId UISession::winId() const
{
    return mainMachineWindow()->winId();
}

void UISession::setPointerShape(const uchar *pShapeData, bool fHasAlpha,
                                uint uXHot, uint uYHot, uint uWidth, uint uHeight)
{
    AssertMsg(pShapeData, ("Shape data must not be NULL!\n"));

    m_fIsValidPointerShapePresent = false;
    const uchar *srcAndMaskPtr = pShapeData;
    uint andMaskSize = (uWidth + 7) / 8 * uHeight;
    const uchar *srcShapePtr = pShapeData + ((andMaskSize + 3) & ~3);
    uint srcShapePtrScan = uWidth * 4;

#if defined (Q_WS_WIN)

    BITMAPV5HEADER bi;
    HBITMAP hBitmap;
    void *lpBits;

    ::ZeroMemory(&bi, sizeof (BITMAPV5HEADER));
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = uWidth;
    bi.bV5Height = - (LONG)uHeight;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    if (fHasAlpha)
        bi.bV5AlphaMask = 0xFF000000;
    else
        bi.bV5AlphaMask = 0;

    HDC hdc = GetDC(NULL);

    /* Create the DIB section with an alpha channel: */
    hBitmap = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&lpBits, NULL, (DWORD) 0);

    ReleaseDC(NULL, hdc);

    HBITMAP hMonoBitmap = NULL;
    if (fHasAlpha)
    {
        /* Create an empty mask bitmap: */
        hMonoBitmap = CreateBitmap(uWidth, uHeight, 1, 1, NULL);
    }
    else
    {
        /* Word aligned AND mask. Will be allocated and created if necessary. */
        uint8_t *pu8AndMaskWordAligned = NULL;

        /* Width in bytes of the original AND mask scan line. */
        uint32_t cbAndMaskScan = (uWidth + 7) / 8;

        if (cbAndMaskScan & 1)
        {
            /* Original AND mask is not word aligned. */

            /* Allocate memory for aligned AND mask. */
            pu8AndMaskWordAligned = (uint8_t *)RTMemTmpAllocZ((cbAndMaskScan + 1) * uHeight);

            Assert(pu8AndMaskWordAligned);

            if (pu8AndMaskWordAligned)
            {
                /* According to MSDN the padding bits must be 0.
                 * Compute the bit mask to set padding bits to 0 in the last byte of original AND mask. */
                uint32_t u32PaddingBits = cbAndMaskScan * 8  - uWidth;
                Assert(u32PaddingBits < 8);
                uint8_t u8LastBytesPaddingMask = (uint8_t)(0xFF << u32PaddingBits);

                Log(("u8LastBytesPaddingMask = %02X, aligned w = %d, width = %d, cbAndMaskScan = %d\n",
                      u8LastBytesPaddingMask, (cbAndMaskScan + 1) * 8, uWidth, cbAndMaskScan));

                uint8_t *src = (uint8_t *)srcAndMaskPtr;
                uint8_t *dst = pu8AndMaskWordAligned;

                unsigned i;
                for (i = 0; i < uHeight; i++)
                {
                    memcpy(dst, src, cbAndMaskScan);

                    dst[cbAndMaskScan - 1] &= u8LastBytesPaddingMask;

                    src += cbAndMaskScan;
                    dst += cbAndMaskScan + 1;
                }
            }
        }

        /* Create the AND mask bitmap: */
        hMonoBitmap = ::CreateBitmap(uWidth, uHeight, 1, 1,
                                     pu8AndMaskWordAligned? pu8AndMaskWordAligned: srcAndMaskPtr);

        if (pu8AndMaskWordAligned)
        {
            RTMemTmpFree(pu8AndMaskWordAligned);
        }
    }

    Assert(hBitmap);
    Assert(hMonoBitmap);
    if (hBitmap && hMonoBitmap)
    {
        DWORD *dstShapePtr = (DWORD *) lpBits;

        for (uint y = 0; y < uHeight; y ++)
        {
            memcpy(dstShapePtr, srcShapePtr, srcShapePtrScan);
            srcShapePtr += srcShapePtrScan;
            dstShapePtr += uWidth;
        }

        ICONINFO ii;
        ii.fIcon = FALSE;
        ii.xHotspot = uXHot;
        ii.yHotspot = uYHot;
        ii.hbmMask = hMonoBitmap;
        ii.hbmColor = hBitmap;

        HCURSOR hAlphaCursor = CreateIconIndirect(&ii);
        Assert(hAlphaCursor);
        if (hAlphaCursor)
        {
            /* Set the new cursor: */
            m_cursor = QCursor(hAlphaCursor);
            if (m_alphaCursor)
                DestroyIcon(m_alphaCursor);
            m_alphaCursor = hAlphaCursor;
            m_fIsValidPointerShapePresent = true;
        }
    }

    if (hMonoBitmap)
        DeleteObject(hMonoBitmap);
    if (hBitmap)
        DeleteObject(hBitmap);

#elif defined (Q_WS_X11) && !defined (VBOX_WITHOUT_XCURSOR)

    XcursorImage *img = XcursorImageCreate(uWidth, uHeight);
    Assert(img);
    if (img)
    {
        img->xhot = uXHot;
        img->yhot = uYHot;

        XcursorPixel *dstShapePtr = img->pixels;

        for (uint y = 0; y < uHeight; y ++)
        {
            memcpy (dstShapePtr, srcShapePtr, srcShapePtrScan);

            if (!fHasAlpha)
            {
                /* Convert AND mask to the alpha channel: */
                uchar byte = 0;
                for (uint x = 0; x < uWidth; x ++)
                {
                    if (!(x % 8))
                        byte = *(srcAndMaskPtr ++);
                    else
                        byte <<= 1;

                    if (byte & 0x80)
                    {
                        /* Linux doesn't support inverted pixels (XOR ops,
                         * to be exact) in cursor shapes, so we detect such
                         * pixels and always replace them with black ones to
                         * make them visible at least over light colors */
                        if (dstShapePtr [x] & 0x00FFFFFF)
                            dstShapePtr [x] = 0xFF000000;
                        else
                            dstShapePtr [x] = 0x00000000;
                    }
                    else
                        dstShapePtr [x] |= 0xFF000000;
                }
            }

            srcShapePtr += srcShapePtrScan;
            dstShapePtr += uWidth;
        }

        /* Set the new cursor: */
        m_cursor = QCursor(XcursorImageLoadCursor(QX11Info::display(), img));
        m_fIsValidPointerShapePresent = true;

        XcursorImageDestroy(img);
    }

#elif defined(Q_WS_MAC)

    /* Create a ARGB image out of the shape data. */
    QImage image  (uWidth, uHeight, QImage::Format_ARGB32);
    const uint8_t* pbSrcMask = static_cast<const uint8_t*> (srcAndMaskPtr);
    unsigned cbSrcMaskLine = RT_ALIGN (uWidth, 8) / 8;
    for (unsigned int y = 0; y < uHeight; ++y)
    {
        for (unsigned int x = 0; x < uWidth; ++x)
        {
           unsigned int color = ((unsigned int*)srcShapePtr)[y*uWidth+x];
           /* If the alpha channel isn't in the shape data, we have to
            * create them from the and-mask. This is a bit field where 1
            * represent transparency & 0 opaque respectively. */
           if (!fHasAlpha)
           {
               if (!(pbSrcMask[x / 8] & (1 << (7 - (x % 8)))))
                   color  |= 0xff000000;
               else
               {
                   /* This isn't quite right, but it's the best we can do I think... */
                   if (color & 0x00ffffff)
                       color = 0xff000000;
                   else
                       color = 0x00000000;
               }
           }
           image.setPixel (x, y, color);
        }
        /* Move one scanline forward. */
        pbSrcMask += cbSrcMaskLine;
    }

    /* Set the new cursor: */
    m_cursor = QCursor(QPixmap::fromImage(image), uXHot, uYHot);
    m_fIsValidPointerShapePresent = true;
    NOREF(srcShapePtrScan);

#else

# warning "port me"

#endif
}

void UISession::reinitMenuPool()
{
    /* Get uisession machine: */
    const CMachine &machine = session().GetConsole().GetMachine();

    /* Availability settings: */
    {
        /* USB Stuff: */
        const CUSBController &usbController = machine.GetUSBController();
        if (   usbController.isNull()
            || !usbController.GetEnabled()
            || !usbController.GetProxyAvailable())
        {
            /* Hide USB menu if controller is NULL or no USB proxy available: */
            uimachine()->actionsPool()->action(UIActionIndex_Menu_USBDevices)->setVisible(false);
        }
        else
        {
            /* Enable/Disable USB menu depending on USB controller: */
            uimachine()->actionsPool()->action(UIActionIndex_Menu_USBDevices)->setEnabled(usbController.GetEnabled());
        }
    }

    /* Prepare some initial settings: */
    {
        /* Initialize CD/FD menus: */
        int iDevicesCountCD = 0;
        int iDevicesCountFD = 0;
        const CMediumAttachmentVector &attachments = machine.GetMediumAttachments();
        foreach (const CMediumAttachment &attachment, attachments)
        {
            if (attachment.GetType() == KDeviceType_DVD)
                ++ iDevicesCountCD;
            if (attachment.GetType() == KDeviceType_Floppy)
                ++ iDevicesCountFD;
        }
        QAction *pOpticalDevicesMenu = uimachine()->actionsPool()->action(UIActionIndex_Menu_OpticalDevices);
        QAction *pFloppyDevicesMenu = uimachine()->actionsPool()->action(UIActionIndex_Menu_FloppyDevices);
        pOpticalDevicesMenu->setData(iDevicesCountCD);
        pOpticalDevicesMenu->setVisible(iDevicesCountCD);
        pFloppyDevicesMenu->setData(iDevicesCountFD);
        pFloppyDevicesMenu->setVisible(iDevicesCountFD);
    }
}

void UISession::preparePowerUp()
{
#ifdef VBOX_WITH_UPDATE_REQUEST
    /* Check for updates if necessary: */
    vboxGlobal().showUpdateDialog(false /* force request? */);
#endif

    /* Notify user about mouse&keyboard auto-capturing: */
    if (vboxGlobal().settings().autoCapture())
        vboxProblem().remindAboutAutoCapture();

    /* Shows first run wizard if necessary: */
    const CMachine &machine = session().GetMachine();
    /* Check if we are in teleportation waiting mode. In that case no first run
     * wizard is necessary. */
    KMachineState state = machine.GetState();
    if (   isFirstTimeStarted()
        && !((   state == KMachineState_PoweredOff
              || state == KMachineState_Aborted
              || state == KMachineState_Teleported)
             && machine.GetTeleporterEnabled()))
    {
        UIFirstRunWzd wzd(mainMachineWindow(), session().GetMachine());
        wzd.exec();
    }
}

#ifdef VBOX_WITH_VIDEOHWACCEL
UIFrameBuffer* UISession::frameBuffer(ulong screenId) const
{
    Assert(screenId < (ulong)m_FrameBufferVector.size());
    return m_FrameBufferVector.value((int)screenId, NULL);
}

int UISession::setFrameBuffer(ulong screenId, UIFrameBuffer* pFrameBuffer)
{
    Assert(screenId < (ulong)m_FrameBufferVector.size());
    if (screenId < (ulong)m_FrameBufferVector.size())
    {
        m_FrameBufferVector[(int)screenId] = pFrameBuffer;
        return VINF_SUCCESS;
    }
    return VERR_INVALID_PARAMETER;
}
#endif
