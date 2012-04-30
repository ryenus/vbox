/* $Id$ */
/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIWizardCloneVDPageBasic2 class implementation
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Global includes: */
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QGroupBox>
#include <QRadioButton>

/* Local includes: */
#include "UIWizardCloneVDPageBasic2.h"
#include "UIWizardCloneVD.h"
#include "VBoxGlobal.h"
#include "QIRichTextLabel.h"

UIWizardCloneVDPage2::UIWizardCloneVDPage2()
{
}

QRadioButton* UIWizardCloneVDPage2::addFormatButton(QVBoxLayout *pFormatsLayout, CMediumFormat medFormat)
{
    /* Check that medium format supports creation: */
    ULONG uFormatCapabilities = medFormat.GetCapabilities();
    if (!(uFormatCapabilities & MediumFormatCapabilities_CreateFixed ||
          uFormatCapabilities & MediumFormatCapabilities_CreateDynamic))
        return 0;

    /* Check that medium format supports creation of virtual hard-disks: */
    QVector<QString> fileExtensions;
    QVector<KDeviceType> deviceTypes;
    medFormat.DescribeFileExtensions(fileExtensions, deviceTypes);
    if (!deviceTypes.contains(KDeviceType_HardDisk))
        return 0;

    /* Create/add corresponding radio-button: */
    QRadioButton *pFormatButton = new QRadioButton(m_pFormatCnt);
    pFormatsLayout->addWidget(pFormatButton);
    return pFormatButton;
}

CMediumFormat UIWizardCloneVDPage2::mediumFormat() const
{
    return m_pFormatButtonGroup->checkedButton() ? m_formats[m_pFormatButtonGroup->checkedId()] : CMediumFormat();
}

void UIWizardCloneVDPage2::setMediumFormat(const CMediumFormat &mediumFormat)
{
    int iPosition = m_formats.indexOf(mediumFormat);
    if (iPosition >= 0)
    {
        m_pFormatButtonGroup->button(iPosition)->click();
        m_pFormatButtonGroup->button(iPosition)->setFocus();
    }
}

UIWizardCloneVDPageBasic2::UIWizardCloneVDPageBasic2()
{
    /* Create widgets: */
    QVBoxLayout *pMainLayout = new QVBoxLayout(this);
    {
        m_pLabel = new QIRichTextLabel(this);
        m_pFormatCnt = new QGroupBox(this);
        {
            m_pFormatCnt->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
            QVBoxLayout *pFormatsLayout = new QVBoxLayout(m_pFormatCnt);
            {
                m_pFormatButtonGroup = new QButtonGroup(this);
                {
                    CSystemProperties systemProperties = vboxGlobal().virtualBox().GetSystemProperties();
                    const QVector<CMediumFormat> &medFormats = systemProperties.GetMediumFormats();
                    for (int i = 0; i < medFormats.size(); ++i)
                    {
                        const CMediumFormat &medFormat = medFormats[i];
                        QString strFormatName(medFormat.GetName());
                        if (strFormatName == "VDI")
                        {
                            QRadioButton *pButton = addFormatButton(pFormatsLayout, medFormat);
                            if (pButton)
                            {
                                m_formats << medFormat;
                                m_formatNames << strFormatName;
                                m_pFormatButtonGroup->addButton(pButton, m_formatNames.size() - 1);
                            }
                        }
                    }
                    for (int i = 0; i < medFormats.size(); ++i)
                    {
                        const CMediumFormat &medFormat = medFormats[i];
                        QString strFormatName(medFormat.GetName());
                        if (strFormatName != "VDI")
                        {
                            QRadioButton *pButton = addFormatButton(pFormatsLayout, medFormat);
                            if (pButton)
                            {
                                m_formats << medFormat;
                                m_formatNames << strFormatName;
                                m_pFormatButtonGroup->addButton(pButton, m_formatNames.size() - 1);
                            }
                        }
                    }
                    m_pFormatButtonGroup->button(0)->click();
                    m_pFormatButtonGroup->button(0)->setFocus();
                }
            }
        }
        pMainLayout->addWidget(m_pLabel);
        pMainLayout->addWidget(m_pFormatCnt);
        pMainLayout->addStretch();
    }

    /* Setup connections: */
    connect(m_pFormatButtonGroup, SIGNAL(buttonClicked(QAbstractButton *)), this, SIGNAL(completeChanged()));

    /* Register classes: */
    qRegisterMetaType<CMediumFormat>();
    /* Register fields: */
    registerField("mediumFormat", this, "mediumFormat");
}

void UIWizardCloneVDPageBasic2::retranslateUi()
{
    /* Translate page: */
    setTitle(UIWizardCloneVD::tr("Virtual disk file type"));

    /* Translate widgets: */
    m_pLabel->setText(UIWizardCloneVD::tr("Please choose the type of file that you would like "
                                          "to use for the new virtual disk. If you do not need "
                                          "to use it with other virtualization software you can "
                                          "leave this setting unchanged."));
    m_pFormatCnt->setTitle(UIWizardCloneVD::tr("File type"));
    QList<QAbstractButton*> buttons = m_pFormatButtonGroup->buttons();
    for (int i = 0; i < buttons.size(); ++i)
    {
        QAbstractButton *pButton = buttons[i];
        pButton->setText(UIWizardCloneVD::fullFormatName(m_formatNames[m_pFormatButtonGroup->id(pButton)]));
    }
}

void UIWizardCloneVDPageBasic2::initializePage()
{
    /* Translate page: */
    retranslateUi();
}

bool UIWizardCloneVDPageBasic2::isComplete() const
{
    /* Make sure medium format is correct: */
    return !mediumFormat().isNull();
}

int UIWizardCloneVDPageBasic2::nextId() const
{
    /* Show variant page only if there is something to show: */
    CMediumFormat medFormat = mediumFormat();
    ULONG uCapabilities = medFormat.GetCapabilities();
    int cTest = 0;
    if (uCapabilities & KMediumFormatCapabilities_CreateDynamic)
        ++cTest;
    if (uCapabilities & KMediumFormatCapabilities_CreateFixed)
        ++cTest;
    if (uCapabilities & KMediumFormatCapabilities_CreateSplit2G)
        ++cTest;
    if (cTest > 1)
        return UIWizardCloneVD::Page3;
    /* Skip otherwise: */
    return UIWizardCloneVD::Page4;
}
