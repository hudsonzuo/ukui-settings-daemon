#ifndef HOUSEKEEPINGMANAGER_H
#define HOUSEKEEPINGMANAGER_H

#include <QObject>
#include <QGSettings/qgsettings.h>
#include <QApplication>


#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include "usd-disk-space.h"

class HousekeepingManager : public QObject
{
    Q_OBJECT
private:
    HousekeepingManager();
    HousekeepingManager(HousekeepingManager&)=delete;
public:
    ~HousekeepingManager();
    static HousekeepingManager *HousekeepingManagerNew();
    bool HousekeepingManagerStart();
    void HousekeepingManagerStop();

public Q_SLOTS:
    void settings_changed_callback(QString);

public:
    void do_cleanup_soon();
    void purge_thumbnail_cache ();
    bool do_cleanup ();
    bool do_cleanup_once ();

private:
    static HousekeepingManager *mHouseManager;
    static DIskSpace *mDisk;
    unsigned int long_term_cb;
    unsigned int short_term_cb;
    QGSettings   *settings;

};

#endif // HOUSEKEEPINGMANAGER_H
