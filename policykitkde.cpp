/*  This file is part of the KDE project
    Copyright (C) 2007-2008 Gökçen Eraslan <gokcen@pardus.org.tr>
    Copyright (C) 2008 Dirk Mueller <mueller@kde.org>
    Copyright (C) 2008 Lubos Lunak <l.lunak@kde.org>
    Copyright (C) 2008 Dario Freddi <drf54321@gmail.com>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.

*/

#include "policykitkde.h"
#include "authenticationagentadaptor.h"

#include <assert.h>
#include <KDebug>
#include <QString>
#include <KInputDialog>
#include <KMessageBox>
#include <KWindowSystem>
#include <KApplication>

#include "qdbusconnection.h"

#include <QSocketNotifier>

//policykit header
#include <polkit-dbus/polkit-dbus.h>
#include <polkit-grant/polkit-grant.h>

#include "authdialog.h"
#include "processwatcher.h"

class PolicyKitKDEHelper
{
  public:
    PolicyKitKDEHelper() : q(0) {}
    ~PolicyKitKDEHelper() { delete q; }
    PolicyKitKDE *q;
};

K_GLOBAL_STATIC(PolicyKitKDEHelper, s_globalPolicyKitKDE)

PolicyKitKDE *PolicyKitKDE::instance()
{
    if (!s_globalPolicyKitKDE->q) {
        new PolicyKitKDE;
    }

    return s_globalPolicyKitKDE->q;
}

//----------------------------------------------------------------------------

PolicyKitKDE::PolicyKitKDE(QObject* parent)
    : QObject(parent)
    , inProgress( false )
{
    Q_ASSERT(!s_globalPolicyKitKDE->q);
    s_globalPolicyKitKDE->q = this;

    kDebug() << "Constructing PolicyKitKDE singleton";

    (void) new AuthenticationAgentAdaptor(this);
    if (!QDBusConnection::sessionBus().registerService("org.freedesktop.PolicyKit.AuthenticationAgent"))
        kError() << "another authentication agent already running";

    if (!QDBusConnection::sessionBus().registerObject("/", this)) {
        kError() << "unable to register service interface to dbus";
    }

    m_context = polkit_context_new();
    if (m_context == NULL)
    {
        kDebug() << "Could not get a new PolKitContext.";
        return;
    }

    polkit_context_set_load_descriptions(m_context);

    polkit_context_set_config_changed( m_context, polkit_config_changed, NULL );
    polkit_context_set_io_watch_functions (m_context, add_context_io_watch, remove_context_io_watch);

    PolKitError* error = NULL;
    if (!polkit_context_init (m_context, &error))
    {
        QString msg("Could not initialize PolKitContext");
        if (polkit_error_is_set(error))
        {
            kError() << msg <<  ": " << polkit_error_get_error_message(error);
            polkit_error_free( error );
        }
        else
            kError() << msg;
    }
    //TODO: polkit_tracker?
    //TODO: add kill_timer?
}

//----------------------------------------------------------------------------

PolicyKitKDE::~PolicyKitKDE()
{
}

//----------------------------------------------------------------------------

bool PolicyKitKDE::ObtainAuthorization(const QString& actionId, uint wid, uint pid)
{
    kDebug() << "Start obtain authorization:" << actionId << wid << pid;

    if( inProgress )
    {
        // TODO this is lame
        sendErrorReply( "pk_auth_in_progress",
            i18n( "Another client is already authenticating, please try again later." ));
        return false;
    }
    inProgress = true;
    obtainedPrivilege = false;
    requireAdmin = false;
    keepPassword = KeepPasswordNo;
    cancelled = false;

    action = polkit_action_new();
    if (action == NULL)
    {
        kError() << "Could not create new polkit action.";
        return false;
    }
    if( !polkit_action_set_action_id(action, actionId.toLatin1()))
    {
        kError() << "Could not set actionid.";
        return false;
    }
    DBusError dbuserror;
    dbus_error_init (&dbuserror);
    DBusConnection *bus = dbus_bus_get (DBUS_BUS_SYSTEM, &dbuserror);
    caller = polkit_caller_new_from_pid(bus, pid, &dbuserror);
    if (caller == NULL)
    {
        kError() << QString("Could not define caller from pid: %1")
            .arg(QDBusError((const DBusError *)&dbuserror).message());
        // TODO this all leaks and is probably pretty paranoid
        return false;
    }
    dbus_connection_unref( bus );

    PolKitPolicyCache *cache = polkit_context_get_policy_cache(m_context);
    if (cache == NULL)
    {
        kWarning() << "Could not get policy cache.";
    //    return false;
    }

    kDebug() << "Getting policy cache entry for an action...";
    PolKitPolicyFileEntry *entry = polkit_policy_cache_get_entry(cache, action);
    if (entry == NULL)
    {
        kWarning() << "Could not get policy entry for action.";
    //    return false;
    }

    kDebug() << "Getting action message...";
    QString actionMessage = QString::fromLocal8Bit(polkit_policy_file_entry_get_action_message(entry));
    if( actionMessage.isEmpty())
    {
        kWarning() << "Could not get action message for action.";
    //    return false;
    }
    else
    {
        kDebug() << "Message of action: " << actionMessage;
    }
    QString vendor = polkit_policy_file_entry_get_action_vendor( entry );
    QString vendorUrl = polkit_policy_file_entry_get_action_vendor_url( entry );
    QPixmap icon = KIconLoader::global()->loadIcon( polkit_policy_file_entry_get_action_icon_name( entry ),
        KIconLoader::NoGroup, KIconLoader::SizeHuge, KIconLoader::DefaultState, QStringList(), NULL, true );
    if( icon.isNull())
        icon = KIconLoader::global()->loadIcon( "dialog-password",
        KIconLoader::NoGroup, KIconLoader::SizeHuge );
    QString appname;
    char tmp[ PATH_MAX ];
    if( polkit_sysdeps_get_exe_for_pid_with_helper( pid, tmp, sizeof( tmp ) - 1 ) < 0 )
        appname = i18n( "Unknown" );
    else
        appname = QString::fromLocal8Bit( tmp );

    dialog = new AuthDialog( actionMessage, icon, appname, actionId, vendor, vendorUrl );
    connect( dialog, SIGNAL( okClicked()), SLOT( dialogAccepted()));
    connect( dialog, SIGNAL( cancelClicked()), SLOT( dialogCancelled()));
    if( wid != 0 )
        KWindowSystem::setMainWindow( dialog, wid );
    else
        kapp->updateUserTimestamp(); // make it get focus unconditionally :-/
    parent_wid = wid;

    grant = polkit_grant_new();
    polkit_grant_set_functions( grant, add_grant_io_watch, add_child_watch, remove_watch,
        conversation_type, conversation_select_admin_user, conversation_pam_prompt_echo_off,
        conversation_pam_prompt_echo_on, conversation_pam_error_msg, conversation_pam_text_info,
        conversation_override_grant_type, conversation_done, this );
    if( !polkit_grant_initiate_auth( grant, action, caller ))
    {
        kError() << "Failed to initiate privilege grant.";
        return false;
    }
    mes = message();
    setDelayedReply( true );
    return false;
}

void PolicyKitKDE::finishObtainPrivilege()
{
    assert( inProgress );
    polkit_grant_unref( grant );
    if( dialog->isVisible() && !cancelled && !obtainedPrivilege )
    {
        dialog->clearPassword();
        // TODO this should probably just show it directly in the dialog, like KPasswordDialog does
        KMessageBox::sorry( dialog, i18n( "Incorrect password, please try again." ));
        grant = polkit_grant_new();
            polkit_grant_set_functions( grant, add_grant_io_watch, add_child_watch, remove_watch,
            conversation_type, conversation_select_admin_user, conversation_pam_prompt_echo_off,
            conversation_pam_prompt_echo_on, conversation_pam_error_msg, conversation_pam_text_info,
            conversation_override_grant_type, conversation_done, this );
        if( !polkit_grant_initiate_auth( grant, action, caller ))
        {
            kError() << "Failed to initiate privilege grant.";
        }
        return;
    }
    polkit_caller_unref( caller );
    polkit_action_unref( action );
    dialog->deleteLater();
    inProgress = false;
    kDebug() << "Finish obtain authorization:" << obtainedPrivilege;
    QDBusConnection::sessionBus().send( mes.createReply( obtainedPrivilege ));
}

void PolicyKitKDE::conversation_type( PolKitGrant* grant, PolKitResult type, void* )
{
    kDebug() << "conversation_type" << grant << type;
    PolicyKitKDE::instance()->requireAdmin = false;
    PolicyKitKDE::instance()->keepPassword = KeepPasswordNo;
    switch( type )
    {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
            PolicyKitKDE::instance()->requireAdmin = true;
            break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
            PolicyKitKDE::instance()->requireAdmin = true;
            PolicyKitKDE::instance()->keepPassword = KeepPasswordSession;
            break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
            PolicyKitKDE::instance()->requireAdmin = true;
            PolicyKitKDE::instance()->keepPassword = KeepPasswordAlways;
            break;
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
            break;
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
            PolicyKitKDE::instance()->keepPassword = KeepPasswordSession;
            break;
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
            PolicyKitKDE::instance()->keepPassword = KeepPasswordAlways;
            break;
        default:
            abort();
    }
}

char* PolicyKitKDE::conversation_select_admin_user(PolKitGrant* grant, char** users, void* )
{
    kDebug() << "conversation_select_admin_user" << grant << users[ 0 ];
    return strdup( users[ 0 ] ); // TODO
}

char* PolicyKitKDE::conversation_pam_prompt_echo_off(PolKitGrant* grant, const char* request, void* )
{
    kDebug() << "conversation_pam_prompt_echo_off" << grant << request;
    // TODO actually use 'request'?
    if( PolicyKitKDE::instance()->requireAdmin )
    {
        PolicyKitKDE::instance()->dialog->setContent( i18n("An application is attempting to perform an action that requires privileges."
            " Authentication as the super user is required to perform this action." ));
        PolicyKitKDE::instance()->dialog->setPasswordPrompt( i18n("Password for root:") );
    }
    else
    {
        PolicyKitKDE::instance()->dialog->setContent( i18n("An application is attempting to perform an action that requires privileges."
                    " Authentication is required to perform this action." ));
        PolicyKitKDE::instance()->dialog->setPasswordPrompt( i18n("Password:") );
        // placeholders
        i18n( "An application is attempting to perform an action that requires privileges."
            " Authentication as one of the users below user is required to perform this action." );
        i18n( "Password for %1" );
    }
    PolicyKitKDE::instance()->dialog->showKeepPassword( PolicyKitKDE::instance()->keepPassword );
    PolicyKitKDE::instance()->dialog->show();
    QEventLoop loop;
    connect( PolicyKitKDE::instance()->dialog, SIGNAL( okClicked()), &loop, SLOT( quit()));
    connect( PolicyKitKDE::instance()->dialog, SIGNAL( cancelClicked()), &loop, SLOT( quit()));
    loop.exec(); // TODO this really sucks, policykit API is blocking
    if( PolicyKitKDE::instance()->cancelled )
        return NULL;
    return strdup( PolicyKitKDE::instance()->dialog->password().toLocal8Bit());
}

void PolicyKitKDE::dialogAccepted()
{
    PolicyKitKDE::instance()->keepPassword = dialog->keepPassword();
    kDebug() << "Password dialog confirmed.";
}

void PolicyKitKDE::dialogCancelled()
{
    PolicyKitKDE::instance()->cancelled = true;
    kDebug() << "Password dialog cancelled.";
    polkit_grant_cancel_auth( grant );
}

char* PolicyKitKDE::conversation_pam_prompt_echo_on(PolKitGrant* grant, const char* request, void* )
{
    kDebug() << "conversation_pam_prompt_echo_on" << grant << request;
    // TODO doesn't set proper parent, and is probably not the right way, but what does actually use this?
    return strdup( KInputDialog::getText( QString(), QString::fromLocal8Bit( request )).toLocal8Bit());
}

void PolicyKitKDE::conversation_pam_error_msg(PolKitGrant* grant, const char* msg, void* )
{
    kDebug() << "conversation_pam_error_msg" << grant << msg;
    KMessageBox::errorWId( PolicyKitKDE::instance()->dialog->isVisible() ? PolicyKitKDE::instance()->dialog->winId() : PolicyKitKDE::instance()->parent_wid,
        QString::fromLocal8Bit( msg ));
}

void PolicyKitKDE::conversation_pam_text_info(PolKitGrant* grant, const char* msg, void* )
{
    kDebug() << "conversation_pam_text_info" << grant << msg;
    KMessageBox::informationWId( PolicyKitKDE::instance()->dialog->isVisible() ? PolicyKitKDE::instance()->dialog->winId() : PolicyKitKDE::instance()->parent_wid,
        QString::fromLocal8Bit( msg ));
}

PolKitResult PolicyKitKDE::conversation_override_grant_type(PolKitGrant* grant, PolKitResult type, void* )
{
    kDebug() << "conversation_override_grant_type" << grant << type;
    bool keep_session = false;
    bool keep_always = false;
    switch( type )
    {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
            break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
            if( PolicyKitKDE::instance()->keepPassword == KeepPasswordSession )
                keep_session = true;
            break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
            if( PolicyKitKDE::instance()->keepPassword == KeepPasswordAlways )
                keep_always = true;
            else if( PolicyKitKDE::instance()->keepPassword == KeepPasswordSession )
                keep_session = true;
            break;
        default:
            abort();
    }
    kDebug() << "Keep password, session:" << keep_session << ", always:" << keep_always;
    PolKitResult ret;
    switch( type )
    {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
            if( keep_session )
                ret = POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION;
            else if( keep_always )
                ret = POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS;
            else
                ret = POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH;
            break;
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_ONE_SHOT:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
            if( keep_session )
                ret = POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION;
            else if( keep_always )
                ret = POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS;
            else
                ret = POLKIT_RESULT_ONLY_VIA_SELF_AUTH;
            break;
        default:
            abort();
    }
    return ret;
}

void PolicyKitKDE::conversation_done(PolKitGrant* grant, polkit_bool_t obtainedPrivilege,
    polkit_bool_t invalidData, void* )
{
    kDebug() << "conversation_done" << grant << obtainedPrivilege << invalidData;
    PolicyKitKDE::instance()->obtainedPrivilege = obtainedPrivilege;
    QTimer::singleShot( 0, PolicyKitKDE::instance(), SLOT( finishObtainPrivilege()));
}

//----------------------------------------------------------------------------

void PolicyKitKDE::watchActivatedGrant(int fd)
{
    Q_ASSERT(m_watches.contains(fd));

//    kDebug() << "watchActivated" << fd;

    polkit_grant_io_func (grant, fd);
}

//----------------------------------------------------------------------------

int PolicyKitKDE::add_grant_io_watch(PolKitGrant* grant, int fd)
{
    kDebug() << "add_watch" << grant << fd;

    QSocketNotifier *notify = new QSocketNotifier(fd, QSocketNotifier::Read, PolicyKitKDE::instance());
    PolicyKitKDE::instance()->m_watches[fd] = notify;

    notify->connect(notify, SIGNAL(activated(int)), PolicyKitKDE::instance(), SLOT(watchActivatedGrant(int)));

    return fd; // use simply the fd as the unique id for the watch
    // TODO this will be insufficient if there will be more watches for the same fd
}


//----------------------------------------------------------------------------

void PolicyKitKDE::remove_grant_io_watch(PolKitGrant* grant, int id)
{
    assert( id > 0 );
    kDebug() << "remove_watch" << grant << id;
    if( !PolicyKitKDE::instance()->m_watches.contains(id))
        return; // policykit likes to do this more than once

    QSocketNotifier* notify = PolicyKitKDE::instance()->m_watches.take(id);
    notify->deleteLater();
    notify->setEnabled( false );
}

//----------------------------------------------------------------------------

void PolicyKitKDE::watchActivatedContext(int fd)
{
    Q_ASSERT(m_watches.contains(fd));

//    kDebug() << "watchActivated" << fd;

    polkit_context_io_func (m_context, fd);
}

//----------------------------------------------------------------------------

int PolicyKitKDE::add_context_io_watch(PolKitContext* context, int fd)
{
    kDebug() << "add_watch" << context << fd;

    QSocketNotifier *notify = new QSocketNotifier(fd, QSocketNotifier::Read, PolicyKitKDE::instance());
    PolicyKitKDE::instance()->m_watches[fd] = notify;

    notify->connect(notify, SIGNAL(activated(int)), PolicyKitKDE::instance(), SLOT(watchActivatedContext(int)));

    return fd; // use simply the fd as the unique id for the watch
}


//----------------------------------------------------------------------------

void PolicyKitKDE::remove_context_io_watch(PolKitContext* context, int id)
{
    assert( id > 0 );
    kDebug() << "remove_watch" << context << id;
    if( !PolicyKitKDE::instance()->m_watches.contains(id))
        return; // policykit likes to do this more than once

    QSocketNotifier* notify = PolicyKitKDE::instance()->m_watches.take(id);
    notify->deleteLater();
    notify->setEnabled( false );
}

//----------------------------------------------------------------------------

int PolicyKitKDE::add_child_watch( PolKitGrant*, pid_t pid )
{
    ProcessWatch *watch = new ProcessWatch(pid);
    connect( watch, SIGNAL( terminated( pid_t, int )), PolicyKitKDE::instance(), SLOT( childTerminated( pid_t, int )));
    // return negative so that remove_watch() can tell io and child watches apart
    return - ProcessWatcher::instance()->add(watch);
}

//----------------------------------------------------------------------------

void PolicyKitKDE::remove_child_watch( PolKitGrant*, int id )
{
    assert( id < 0 );
    ProcessWatcher::instance()->remove( -id );
}

//----------------------------------------------------------------------------

void PolicyKitKDE::childTerminated( pid_t pid, int exitStatus )
{
    polkit_grant_child_func( grant, pid, exitStatus );
}

//----------------------------------------------------------------------------

void PolicyKitKDE::remove_watch( PolKitGrant* grant, int id )
{
    if( id > 0 ) // io watches are +, child watches are -
        remove_grant_io_watch( grant, id );
    else
        remove_child_watch( grant, id );
}

//----------------------------------------------------------------------------
void PolicyKitKDE::polkit_config_changed( PolKitContext* context, void* )
{
    kDebug() << "polkit_config_changed" << context;
    // Nothing to do here it seems (?).
}
