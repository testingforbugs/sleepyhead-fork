/*

SleepLib ResMed Loader Implementation

Author: Mark Watkins <jedimark64@users.sourceforge.net>
License: GPL
*/


#include <QApplication>
#include <QString>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProgressBar>
#include <QDebug>
#include <cmath>

#include "resmed_loader.h"
#include "SleepLib/session.h"
#include "SleepLib/calcs.h"

extern QProgressBar *qprogress;
QHash<int,QString> RMS9ModelMap;
QHash<ChannelID, QVector<QString> > resmed_codes;

// Looks up foreign language Signal names that match this channelID
EDFSignal * EDFParser::lookupSignal(ChannelID ch)
{
    QHash<ChannelID, QVector<QString> >::iterator ci;
    QHash<QString,EDFSignal *>::iterator jj;
    ci=resmed_codes.find(ch);
    if (ci==resmed_codes.end()) return NULL;
    for (int i=0;i<ci.value().size();i++) {
        jj=lookup.find(ci.value()[i]);
        if (jj==lookup.end()) continue;
        return jj.value();
    }
    return NULL;
}

EDFParser::EDFParser(QString name)
{
    buffer=NULL;
    Open(name);
}
EDFParser::~EDFParser()
{
    QVector<EDFSignal *>::iterator s;
    for (s=edfsignals.begin();s!=edfsignals.end();s++) {
        if ((*s)->data) delete [] (*s)->data;
        delete *s;
    }
    if (buffer) delete [] buffer;
}
qint16 EDFParser::Read16()
{
    unsigned char *buf=(unsigned char *)buffer;
    if (pos>=filesize) return 0;
    qint16 res=*(qint16 *)&buf[pos];
    //qint16 res=(buf[pos] ^128)<< 8 | buf[pos+1] ^ 128;
    pos+=2;
    return res;
}
QString EDFParser::Read(int si)
{
    QString str;
    if (pos>=filesize) return "";
    for (int i=0;i<si;i++) {
        str+=buffer[pos++];
    }
    return str.trimmed();
}
bool EDFParser::Parse()
{
    bool ok;
    QString temp,temp2;

    version=QString::fromAscii(header.version,8).toLong(&ok);
    if (!ok)
        return false;

    //patientident=QString::fromAscii(header.patientident,80);
    recordingident=QString::fromAscii(header.recordingident,80); // Serial number is in here..
    int snp=recordingident.indexOf("SRN=");
    serialnumber.clear();
    /*char * idx=index(header.recordingident,'=');
    idx++;
    for (int i=0;i<16;++i) {
        if (*idx==0x20) break;
        serialnumber+=*idx;
        ++idx;
    } */

    for (int i=snp+4;i<recordingident.length();i++) {
        if (recordingident[i]==' ')
            break;
        serialnumber+=recordingident[i];
    }
    QDateTime startDate=QDateTime::fromString(QString::fromAscii(header.datetime,16),"dd.MM.yyHH.mm.ss");
    //startDate.toTimeSpec(Qt::UTC);
    QDate d2=startDate.date();
    if (d2.year()<2000) {
        d2.setYMD(d2.year()+100,d2.month(),d2.day());
        startDate.setDate(d2);
    }
    if (!startDate.isValid()) {
        qDebug() << "Invalid date time retreieved parsing EDF File " << filename;
        return false;
    }
    startdate=qint64(startDate.toTime_t())*1000L;
    //startdate-=timezoneOffset();

    //qDebug() << startDate.toString("yyyy-MM-dd HH:mm:ss");

    num_header_bytes=QString::fromAscii(header.num_header_bytes,8).toLong(&ok);
    if (!ok)
        return false;
    //reserved44=QString::fromAscii(header.reserved,44);
    num_data_records=QString::fromAscii(header.num_data_records,8).toLong(&ok);
    if (!ok)
        return false;

    dur_data_record=QString::fromAscii(header.dur_data_records,8).toDouble(&ok)*1000.0;
    if (!ok)
        return false;
    num_signals=QString::fromAscii(header.num_signals,4).toLong(&ok);
    if (!ok)
        return false;

    enddate=startdate+dur_data_record*qint64(num_data_records);
   // if (dur_data_record==0)
     //   return false;

    // this could be loaded quicker by transducer_type[signal] etc..

    for (int i=0;i<num_signals;i++) {
        EDFSignal *signal=new EDFSignal;
        edfsignals.push_back(signal);
        signal->data=NULL;
        edfsignals[i]->label=Read(16);
        lookup[edfsignals[i]->label]=signal;
    }

    for (int i=0;i<num_signals;i++) edfsignals[i]->transducer_type=Read(80);

    for (int i=0;i<num_signals;i++) edfsignals[i]->physical_dimension=Read(8);
    for (int i=0;i<num_signals;i++) edfsignals[i]->physical_minimum=Read(8).toDouble(&ok);
    for (int i=0;i<num_signals;i++) edfsignals[i]->physical_maximum=Read(8).toDouble(&ok);
    for (int i=0;i<num_signals;i++) edfsignals[i]->digital_minimum=Read(8).toDouble(&ok);
    for (int i=0;i<num_signals;i++) {
        EDFSignal & e=*edfsignals[i];
        e.digital_maximum=Read(8).toDouble(&ok);
        e.gain=(e.physical_maximum-e.physical_minimum)/(e.digital_maximum-e.digital_minimum);
        e.offset=0;
    }

    for (int i=0;i<num_signals;i++) edfsignals[i]->prefiltering=Read(80);
    for (int i=0;i<num_signals;i++) edfsignals[i]->nr=Read(8).toLong(&ok);
    for (int i=0;i<num_signals;i++) edfsignals[i]->reserved=Read(32);

    // allocate the buffers
    for (int i=0;i<num_signals;i++) {
        //qDebug//cout << "Reading signal " << signals[i]->label << endl;
        EDFSignal & sig=*edfsignals[i];

        long recs=sig.nr * num_data_records;
        if (num_data_records<0)
            return false;
        sig.data=new qint16 [recs];
        sig.pos=0;
    }

    for (int x=0;x<num_data_records;x++) {
        for (int i=0;i<num_signals;i++) {
            EDFSignal & sig=*edfsignals[i];
            memcpy((char *)&sig.data[sig.pos],(char *)&buffer[pos],sig.nr*2);
            sig.pos+=sig.nr;
            pos+=sig.nr*2;
            // big endian will probably screw up without this..
            /*for (int j=0;j<sig.nr;j++) {
                qint16 t=Read16();
                sig.data[sig.pos++]=t;
            } */
        }
    }

    return true;
}
bool EDFParser::Open(QString name)
{
    QFile f(name);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (!f.isReadable()) return false;
    filename=name;
    filesize=f.size();
    datasize=filesize-EDFHeaderSize;
    if (datasize<0) return false;

    //Urk.. This needs fixing for VC++, as it doesn't have packed attribute type..

    f.read((char *)&header,EDFHeaderSize);
    //qDebug() << "Opening " << name;
    buffer=new char [datasize];
    f.read(buffer,datasize);
    f.close();
    pos=0;
    return true;
}

ResmedLoader::ResmedLoader()
{
}
ResmedLoader::~ResmedLoader()
{
}

Machine *ResmedLoader::CreateMachine(QString serial,Profile *profile)
{
    if (!profile) return NULL;
    QList<Machine *> ml=profile->GetMachines(MT_CPAP);
    bool found=false;
    QList<Machine *>::iterator i;
    for (i=ml.begin(); i!=ml.end(); i++) {
        if (((*i)->GetClass()==resmed_class_name) && ((*i)->properties[STR_PROP_Serial]==serial)) {
            ResmedList[serial]=*i; //static_cast<CPAP *>(*i);
            found=true;
            break;
        }
    }
    if (found) return *i;

    qDebug() << "Create ResMed Machine" << serial;
    Machine *m=new CPAP(profile,0);
    m->SetClass(resmed_class_name);

    ResmedList[serial]=m;
    profile->AddMachine(m);

    m->properties[STR_PROP_Serial]=serial;
    m->properties[STR_PROP_Brand]=STR_MACH_ResMed;
    QString a;
    a.sprintf("%i",resmed_data_version);
    m->properties[STR_PROP_DataVersion]=a;
    m->properties[STR_PROP_Path]="{"+STR_GEN_DataFolder+"}/"+m->GetClass()+"_"+serial+"/";

    return m;

}

long event_cnt=0;

int ResmedLoader::Open(QString & path,Profile *profile)
{
    const QString datalog="DATALOG";
    const QString idfile="Identification.";
    const QString strfile="STR.";

    const QString ext_TGT="tgt";
    const QString ext_CRC="crc";
    const QString ext_EDF="edf";

    QString serial;                 // Serial number
    QString key,value;
    QString line;
    QString newpath;
    QString filename;

    QHash<QString,QString> idmap;   // Temporary properties hash

    // Strip off end "/" if any
    if (path.endsWith("/"))
        path=path.section("/",0,-2);

    // Strip off DATALOG from path, and set newpath to the path contianing DATALOG
    if (path.endsWith(datalog)) {
        newpath=path+"/";
        path=path.section("/",0,-2);
    } else {
        newpath=path+"/"+datalog+"/";
    }

    // Add separator back
    path+="/";

    // Check DATALOG folder exists and is readable
    if (!QDir().exists(newpath))
        return 0;

    ///////////////////////////////////////////////////////////////////////////////////
    // Parse Identification.tgt file (containing serial number and machine information)
    ///////////////////////////////////////////////////////////////////////////////////
    filename=path+idfile+ext_TGT;
    QFile f(filename);
    // Abort if this file is dodgy..
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return 0;

    // Parse # entries into idmap.
    while (!f.atEnd()) {
        line=f.readLine().trimmed();
        if (!line.isEmpty()) {
            key=line.section(" ",0,0);
            value=line.section(" ",1);
            key=key.section("#",1);
            if (key=="SRN") {
                key=STR_PROP_Serial;
                serial=value;
            }
            idmap[key]=value;
        }
    }
    f.close();

    // Abort if no serial number
    if (serial.isEmpty()) {
        qDebug() << "S9 Data card has no valid serial number in Indentification.tgt";
        return 0;
    }

    // Early check for STR.edf file, so we can early exit before creating faulty machine record.
    QString strpath=path+strfile+ext_EDF;  // STR.edf file
    f.setFileName(strpath);
    if (!f.exists()) {
        qDebug() << "Missing STR.edf file";
        return 0;
    }

    ///////////////////////////////////////////////////////////////////////////////////
    // Create machine object (unless it's already registered)
    ///////////////////////////////////////////////////////////////////////////////////
    Machine *m=CreateMachine(serial,profile);

    ///////////////////////////////////////////////////////////////////////////////////
    // Parse the idmap into machine objects properties, (overwriting any old values)
    ///////////////////////////////////////////////////////////////////////////////////
    for (QHash<QString,QString>::iterator i=idmap.begin();i!=idmap.end();i++) {
        m->properties[i.key()]=i.value();

        if (i.key()=="PCD") { // Lookup Product Code for real model string
            bool ok;
            int j=i.value().toInt(&ok);
            if (ok) m->properties[STR_PROP_Model]=RMS9ModelMap[j];
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////
    // Open and Parse STR.edf file
    ///////////////////////////////////////////////////////////////////////////////////
    EDFParser stredf(strpath);
    if (!stredf.Parse()) {
        qDebug() << "Faulty file" << strfile;
        return 0;
    }
    if (stredf.serialnumber!=serial) {
        qDebug() << "Identification.tgt Serial number doesn't match STR.edf!";
    }


    // Creating early as we need the object
    QDir dir(newpath);

    ///////////////////////////////////////////////////////////////////////////////////
    // Create the backup folder for storing a copy of everything in..
    ///////////////////////////////////////////////////////////////////////////////////
    QString backup_path=PROFILE.Get(m->properties[STR_PROP_Path])+"Backup/";
    if (!dir.exists(backup_path)) {
        if (!dir.mkpath(backup_path+datalog)) {
            qDebug() << "Could not create S9 backup directory :-/";
        }
    }

    // Copy Identification files to backup folder
    QFile::copy(path+idfile+ext_TGT,backup_path+idfile+ext_TGT);
    QFile::copy(path+idfile+ext_CRC,backup_path+idfile+ext_CRC);

    // Copy STR files to backup folder
    QFile::copy(strpath,backup_path+strfile+ext_EDF);
    QFile::copy(path+strfile+ext_CRC,backup_path+strfile+ext_CRC);

    ///////////////////////////////////////////////////////////////////////////////////
    // Process the actual STR.edf data
    ///////////////////////////////////////////////////////////////////////////////////

    qint64 duration=stredf.GetNumDataRecords()*stredf.GetDuration();
    int days=duration/86400000L;

    //QDateTime dt1=QDateTime::fromTime_t(stredf.startdate/1000L);
    //QDateTime dt2=QDateTime::fromTime_t(stredf.enddate/1000L);
    //QDate dd1=dt1.date();
    //QDate dd2=dt2.date();

//    for (int s=0;s<stredf.GetNumSignals();s++) {
//        EDFSignal & es=*stredf.edfsignals[s];
//        long recs=es.nr*stredf.GetNumDataRecords();
//        //qDebug() << "STREDF:" << es.label << recs;
//    }

    // Process STR.edf and find first and last time for each day

    QVector<qint8> dayused;
    dayused.resize(days);
    QList<SessionID> strfirst;
    QList<SessionID> strlast;
    QList<int> strday;
    QList<bool> dayfoo;

    QHash<qint16,QList<time_t> > daystarttimes;
    QHash<qint16,QList<time_t> > dayendtimes;
    qint16 on,off;
    qint16 o1[10],o2[10];
    time_t st,et;
    time_t time=stredf.startdate/1000L; // == 12pm on first day
    for (int i=0;i<days;i++) {
        EDFSignal *maskon=stredf.lookup["Mask On"];
        EDFSignal *maskoff=stredf.lookup["Mask Off"];
        int j=i*10;

        // Counts for on and off don't line up, and I'm not sure why
        // The extra 'off' always seems to start with a 1 at the beginning
        // A signal it's carried over from the day before perhaps? (noon boundary)
        int ckon=0,ckoff=0;
        for (int k=0;k<10;k++) {
            on=maskon->data[j+k];
            off=maskoff->data[j+k];
            o1[k]=on;
            o2[k]=off;
            if (on >= 0) ckon++;
            if (off >= 0) ckoff++;
        }

        // set to true if day starts with machine running
        int offset=ckoff-ckon;
        dayfoo.push_back(offset>0);

        st=0,et=0;
        time_t l,f;

        // Find the Min & Max times for this day
        for (int k=0;k<ckon;k++) {
            on=o1[k];
            off=o2[k+offset];
            f=time+on*60;
            l=time+off*60;
            daystarttimes[i].push_back(f);
            dayendtimes[i].push_back(l);

            if (!st || (st > f)) st=f;
            if (!et || (et < l)) et=l;
        }
        strfirst.push_back(st);
        strlast.push_back(et);
        strday.push_back(i);
        dayused[i]=ckon;
        time+=86400;
    }

    // reset time to first day
    time=stredf.startdate/1000;

    ///////////////////////////////////////////////////////////////////////////////////
    // Open DATALOG file and build list of session files
    ///////////////////////////////////////////////////////////////////////////////////

    dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);
    QFileInfoList flist=dir.entryInfoList();

    QString ext,rest,datestr;//,s,codestr;
    SessionID sessionid;
    QDateTime date;
    int size=flist.size();

    sessfiles.clear();

    // For each file in filelist...
    for (int i=0;i<size;i++) {
        QFileInfo fi=flist.at(i);
        filename=fi.fileName();

        // Forget about it if it can't be read.
        if (!fi.isReadable())
            continue;

        // Check the file extnsion
        ext=filename.section(".",1).toLower();
        if (ext!=ext_EDF) continue;

        // Split the filename into components
        rest=filename.section(".",0,0);
        datestr=filename.section("_",0,1);

        // Take the filename's date, and convert it to epoch to form the sessionID.
        date=QDateTime::fromString(datestr,"yyyyMMdd_HHmmss");
        if (!date.isValid())
            continue; // Skip file if dates invalid

        sessionid=date.toTime_t();

        ////////////////////////////////////////////////////////////////////////////////////////////
        // Resmed bugs up on the session filenames.. 1 or 2 seconds either way
        // Moral of the story, when writing firmware and saving in batches, use the same datetimes.
        ////////////////////////////////////////////////////////////////////////////////////////////
        if (sessfiles.find(sessionid)==sessfiles.end()) {
            if (sessfiles.find(sessionid+2)!=sessfiles.end()) sessionid+=2;
            else if (sessfiles.find(sessionid+1)!=sessfiles.end()) sessionid++;
            else if (sessfiles.find(sessionid-1)!=sessfiles.end()) sessionid--;
            else if (sessfiles.find(sessionid-2)!=sessfiles.end()) sessionid-=2;
        }

        // Push current filename to sanitized by-session list
        sessfiles[sessionid].push_back(rest);

        // Update the progress bar
        if (qprogress) qprogress->setValue((float(i+1)/float(size)*10.0));
        QApplication::processEvents();
    }

    QString fn;
    Session *sess;
    int cnt=0;
    size=sessfiles.size();

    QHash<SessionID,int> sessday;


    /////////////////////////////////////////////////////////////////////////////
    // Scan over file list and knock out of dayused list
    /////////////////////////////////////////////////////////////////////////////
    for (QMap<SessionID,QVector<QString> >::iterator si=sessfiles.begin();si!=sessfiles.end();si++) {
        sessionid=si.key();

        // Earliest possible day number
        int edn=((sessionid-time)/86400)-1;
        if (edn<0) edn=0;

        // Find real day number from str.edf mask on/off data.
        int dn=-1;
        for (int j=edn;j<strfirst.size();j++){
            time_t st=strfirst.at(j);
            time_t et=strlast.at(j);
            if (sessionid>=st) {
                if (sessionid<(et+300)) {
                    dn=j;
                    break;
                }
            }
        }
        if (dn>=0) {
            dayused[dn]=0;
        }
    }

    EDFSignal *sig;

    /////////////////////////////////////////////////////////////////////////////
    // For all days not in session lists, (to get at days without data records)
    /////////////////////////////////////////////////////////////////////////////
    for (int dn=0;dn<days;dn++) {
        if (!dayused[dn]) continue; // Skip days with loadable data.

        if (!daystarttimes.contains(dn)) continue;
        int j;
        int scnt=daystarttimes[dn].size();

        sess=NULL;
        // For each mask on/off segment.
        for (j=0;j<scnt;j++) {
            st=daystarttimes[dn].at(j);

            // Skip if session already exists
            if (m->SessionExists(st))
                continue;

            et=dayendtimes[dn].at(j);

            // Create session
            sess=new Session(m,st);
            sess->really_set_first(qint64(st)*1000L);
            sess->really_set_last(qint64(et)*1000L);
            sess->SetChanged(true);
            m->AddSession(sess,profile);

        }
        // Add the actual data to the last session
        EventDataType tmp,dur;
        if (sess) {
            /////////////////////////////////////////////////////////////////////
            // CPAP Mode
            /////////////////////////////////////////////////////////////////////
            int mode;
            sig=stredf.lookupSignal(CPAP_Mode);
            if (sig) {
                mode=sig->data[dn];
            } else mode=0;


            /////////////////////////////////////////////////////////////////////
            // EPR Settings
            /////////////////////////////////////////////////////////////////////
            sess->settings[CPAP_PresReliefType]=PR_EPR;

            // Note: AutoSV machines don't have both fields
            sig=stredf.lookupSignal(RMS9_EPR);
            if (sig) {
                int i=sig->data[dn];
                sess->settings[CPAP_PresReliefMode]=i;
            }
            sig=stredf.lookupSignal(RMS9_EPRSet);
            if (sig)  {
                sess->settings[CPAP_PresReliefSet]=sig->data[dn];
            }


            /////////////////////////////////////////////////////////////////////
            // Set Min & Max pressures depending on CPAP mode
            /////////////////////////////////////////////////////////////////////
            if (mode==0) {
                sess->settings[CPAP_Mode]=MODE_CPAP;
                sig=stredf.lookupSignal(RMS9_SetPressure); // ?? What's meant by Set Pressure?
                if (sig) {
                    EventDataType pressure=sig->data[dn]*sig->gain;
                    sess->settings[CPAP_PressureMin]=pressure;
                }
            } else { // VPAP or Auto
                if (mode>5) {
                    if (mode>=7)
                        sess->settings[CPAP_Mode]=MODE_ASV;
                    else
                        sess->settings[CPAP_Mode]=MODE_BIPAP;

                    EventDataType tmp,epap=0,ipap=0;
                    if (stredf.lookup.contains("EPAP")) {
                        sig=stredf.lookup["EPAP"];
                        epap=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_EPAP]=epap;
                        sess->setMin(CPAP_EPAP,epap);
                    }
                    if (stredf.lookup.contains("IPAP")) {
                        sig=stredf.lookup["IPAP"];
                        ipap=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_IPAP]=ipap;
                    }
                    if (stredf.lookup.contains("PS")) {
                        sig=stredf.lookup["PS"];
                        tmp=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PS]=tmp; // technically this is IPAP-EPAP
                        if (!ipap) {
                            // not really possible. but anyway, just in case..
                            sess->settings[CPAP_IPAP]=epap+tmp;
                        }
                    }
                    if (stredf.lookup.contains("Min PS")) {
                        sig=stredf.lookup["Min PS"];
                        tmp=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PSMin]=tmp;
                        sess->settings[CPAP_IPAPLo]=epap+tmp;
                        sess->setMin(CPAP_IPAP,epap+tmp);
                    }
                    if (stredf.lookup.contains("Max PS")) {
                        sig=stredf.lookup["Max PS"];
                        tmp=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PSMax]=tmp;
                        sess->settings[CPAP_IPAPHi]=epap+tmp;
                    }
                    if (stredf.lookup.contains("RR")) { // Is this a setting to force respiratory rate on S/T machines?
                        sig=stredf.lookup["RR"];
                        tmp=sig->data[dn];
                        sess->settings[CPAP_RespRate]=tmp*sig->gain;
                    }

                    if (stredf.lookup.contains("Easy-Breathe")) {
                        sig=stredf.lookup["Easy-Breathe"];
                        tmp=sig->data[dn]*sig->gain;

                        sess->settings[CPAP_PresReliefSet]=tmp;
                        sess->settings[CPAP_PresReliefType]=(int)PR_EASYBREATHE;
                        sess->settings[CPAP_PresReliefMode]=(int)PM_FullTime;
                    }

                } else {
                    sess->settings[CPAP_Mode]=MODE_APAP;
                    sig=stredf.lookupSignal(CPAP_PressureMin);
                    if (sig) {
                        EventDataType pressure=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PressureMin]=pressure;
                        sess->setMin(CPAP_Pressure,pressure);
                    }
                    sig=stredf.lookupSignal(CPAP_PressureMax);
                    if (sig) {
                        EventDataType pressure=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PressureMax]=pressure;
                        sess->setMax(CPAP_Pressure,pressure);
                    }

                }
            }

            EventDataType valmed=0,valmax=0,val95=0;

            if (stredf.lookup.contains("Leak Med")) {
                sig=stredf.lookup["Leak Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_Leak,valmed*sig->gain*60.0);
                sess->m_gain[CPAP_Leak]=sig->gain*60.0;
                sess->m_valuesummary[CPAP_Leak][valmed]=50;
            }
            if (stredf.lookup.contains("Leak 95")) {
                sig=stredf.lookup["Leak 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_Leak,val95*sig->gain*60.0);
                sess->m_valuesummary[CPAP_Leak][val95]=45;
            }
            if (stredf.lookup.contains("Leak Max")) {
                sig=stredf.lookup["Leak Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_Leak,valmax*sig->gain*60.0);
                sess->m_valuesummary[CPAP_Leak][valmax]=5;
            }

            if (stredf.lookup.contains("Min Vent Med")) {
                sig=stredf.lookup["Min Vent Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_MinuteVent,valmed*sig->gain);
                sess->m_gain[CPAP_MinuteVent]=sig->gain;
                sess->m_valuesummary[CPAP_MinuteVent][valmed]=50;
            }
            if (stredf.lookup.contains("Min Vent 95")) {
                sig=stredf.lookup["Min Vent 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_MinuteVent,val95*sig->gain);
                sess->m_valuesummary[CPAP_MinuteVent][val95]=45;
            }
            if (stredf.lookup.contains("Min Vent Max")) {
                sig=stredf.lookup["Min Vent Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_MinuteVent,valmax*sig->gain);
                sess->m_valuesummary[CPAP_MinuteVent][valmax]=5;
            }
            if (stredf.lookup.contains("RR Med")) {
                sig=stredf.lookup["RR Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_RespRate,valmed*sig->gain);
                sess->m_gain[CPAP_RespRate]=sig->gain;
                sess->m_valuesummary[CPAP_RespRate][valmed]=50;
            }
            if (stredf.lookup.contains("RR 95")) {
                sig=stredf.lookup["RR 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_RespRate,val95*sig->gain);
                sess->m_valuesummary[CPAP_RespRate][val95]=45;
            }
            if (stredf.lookup.contains("RR Max")) {
                sig=stredf.lookup["RR Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_RespRate,valmax*sig->gain);
                sess->m_valuesummary[CPAP_RespRate][valmax]=5;
            }

            if (stredf.lookup.contains("Tid Vol Med")) {
                sig=stredf.lookup["Tid Vol Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_TidalVolume,valmed*sig->gain);
                sess->m_gain[CPAP_TidalVolume]=sig->gain;
                sess->m_valuesummary[CPAP_TidalVolume][valmed]=50;
            }
            if (stredf.lookup.contains("Tid Vol 95")) {
                sig=stredf.lookup["Tid Vol 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_TidalVolume,val95*sig->gain);
                sess->m_valuesummary[CPAP_TidalVolume][val95]=45;
            }
            if (stredf.lookup.contains("Tid Vol Max")) {
                sig=stredf.lookup["Tid Vol Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_TidalVolume,valmax*sig->gain);
                sess->m_valuesummary[CPAP_TidalVolume][valmax]=5;
            }

            if (stredf.lookup.contains("Targ Vent Med")) {
                sig=stredf.lookup["Targ Vent Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_TgMV,valmed*sig->gain);
                sess->m_gain[CPAP_TgMV]=sig->gain;
                sess->m_valuesummary[CPAP_TgMV][valmed]=50;
            }
            if (stredf.lookup.contains("Targ Vent 95")) {
                sig=stredf.lookup["Targ Vent 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_TgMV,val95*sig->gain);
                sess->m_valuesummary[CPAP_TgMV][val95]=45;
            }
            if (stredf.lookup.contains("Targ Vent Max")) {
                sig=stredf.lookup["Targ Vent Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_TgMV,valmax*sig->gain);
                sess->m_valuesummary[CPAP_TgMV][valmax]=5;
            }


            if (stredf.lookup.contains("I:E Med")) {
                sig=stredf.lookup["I:E Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_IE,valmed*sig->gain);
                sess->m_gain[CPAP_IE]=sig->gain;
                sess->m_valuesummary[CPAP_IE][valmed]=50;
            }
            if (stredf.lookup.contains("I:E 95")) {
                sig=stredf.lookup["I:E 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_IE,val95*sig->gain);
                sess->m_valuesummary[CPAP_IE][val95]=45;
            }
            if (stredf.lookup.contains("I:E Max")) {
                sig=stredf.lookup["I:E Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_IE,valmax*sig->gain);
                sess->m_valuesummary[CPAP_IE][valmax]=5;
            }



            if (stredf.lookup.contains("Mask Pres Med")) {
                sig=stredf.lookup["Mask Pres Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_Pressure,valmed*sig->gain);
                sess->m_gain[CPAP_Pressure]=sig->gain;
                sess->m_valuesummary[CPAP_Pressure][valmed]=50;
            }
            if (stredf.lookup.contains("Mask Pres 95")) {
                sig=stredf.lookup["Mask Pres 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_Pressure,val95*sig->gain);
                sess->m_valuesummary[CPAP_Pressure][val95]=45;
            }
            if (stredf.lookup.contains("Mask Pres Max")) {
                sig=stredf.lookup["Mask Pres Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_Pressure,valmax*sig->gain);
                sess->m_valuesummary[CPAP_Pressure][valmax]=5;
            }

            if (stredf.lookup.contains("Insp Pres Med")) {
                sig=stredf.lookup["Insp Pres Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_IPAP,valmed*sig->gain);
                sess->m_gain[CPAP_IPAP]=sig->gain;
                sess->m_valuesummary[CPAP_IPAP][valmed]=50;
            }
            if (stredf.lookup.contains("Insp Pres 95")) {
                sig=stredf.lookup["Insp Pres 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_IPAP,val95*sig->gain);
                sess->m_valuesummary[CPAP_IPAP][val95]=45;
            }
            if (stredf.lookup.contains("Insp Pres Max")) {
                sig=stredf.lookup["Insp Pres Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_IPAP,valmax*sig->gain);
                sess->m_valuesummary[CPAP_IPAP][valmax]=5;
            }
            if (stredf.lookup.contains("Exp Pres Med")) {
                sig=stredf.lookup["Exp Pres Med"];
                valmed=sig->data[dn];
                sess->setMedian(CPAP_EPAP,valmed*sig->gain);
                sess->m_gain[CPAP_EPAP]=sig->gain;
                sess->m_valuesummary[CPAP_EPAP][valmed]=50;
            }
            if (stredf.lookup.contains("Exp Pres 95")) {
                sig=stredf.lookup["Exp Pres 95"];
                val95=sig->data[dn];
                sess->set95p(CPAP_EPAP,val95*sig->gain);
                sess->m_valuesummary[CPAP_EPAP][val95]=45;
            }
            if (stredf.lookup.contains("Exp Pres Max")) {
                sig=stredf.lookup["Exp Pres Max"];
                valmax=sig->data[dn];
                sess->setMax(CPAP_EPAP,valmax*sig->gain);
                sess->m_valuesummary[CPAP_EPAP][valmax]=5;
            }

            if (stredf.lookup.contains("Mask Dur")) {
                sig=stredf.lookup["Mask Dur"];
                dur=sig->data[dn]*sig->gain;
            }
            if (stredf.lookup.contains("OAI")) {
                sig=stredf.lookup["OAI"];
                tmp=sig->data[dn]*sig->gain;
                sess->setCph(CPAP_Obstructive,tmp);
                sess->setCount(CPAP_Obstructive,tmp*(dur/60.0));
            }
            if (stredf.lookup.contains("HI")) {
                sig=stredf.lookup["HI"];
                tmp=sig->data[dn]*sig->gain;
                sess->setCph(CPAP_Hypopnea,tmp);
                sess->setCount(CPAP_Hypopnea,tmp*(dur/60.0));
            }
            if (stredf.lookup.contains("UAI")) {
                sig=stredf.lookup["UAI"];
                tmp=sig->data[dn]*sig->gain;
                sess->setCph(CPAP_Apnea,tmp);
                sess->setCount(CPAP_Apnea,tmp*(dur/60.0));
            }
            if (stredf.lookup.contains("CAI")) {
                sig=stredf.lookup["CAI"];
                tmp=sig->data[dn]*sig->gain;
                sess->setCph(CPAP_ClearAirway,tmp);
                sess->setCount(CPAP_ClearAirway,tmp*(dur/60.0));
            }



        }

    }

    /////////////////////////////////////////////////////////////////////////////
    // Scan through new file list and import sessions
    /////////////////////////////////////////////////////////////////////////////
    for (QMap<SessionID,QVector<QString> >::iterator si=sessfiles.begin();si!=sessfiles.end();si++) {
        sessionid=si.key();

        // Skip file if already imported
        if (m->SessionExists(sessionid))
            continue;

        // Create the session
        sess=new Session(m,sessionid);

        // Process EDF File List
        for (int i=0;i<si.value().size();++i) {
            QString filename=si.value()[i]+".";
            QString fullpath=newpath+filename;

            // Copy the EDF file to the backup folder
            QString backup_file=backup_path+datalog+"/"+filename;
            QFile().copy(fullpath+ext_EDF, backup_file+ext_EDF);
            QFile().copy(fullpath+ext_CRC, backup_file+ext_CRC);

            fullpath+=ext_EDF;
            EDFParser edf(fullpath);

            // Parse the actual file
            if (!edf.Parse())
                continue;

            // Give a warning if doesn't match the machine serial number in Identification.tgt
            if (edf.serialnumber!=serial) {
                qDebug() << "edf Serial number doesn't match Identification.tgt";
            }

            fn=fullpath.section("_",-1).toLower();

            if (fn=="eve.edf") LoadEVE(sess,edf);
            else if (fn=="pld.edf") LoadPLD(sess,edf);
            else if (fn=="brp.edf") LoadBRP(sess,edf);
            else if (fn=="sad.edf") LoadSAD(sess,edf);
        }
        if (qprogress) qprogress->setValue(10.0+(float(++cnt)/float(size)*90.0));
        QApplication::processEvents();

        if (!sess) continue;
        if (!sess->first()) {
            delete sess;
            continue;
        } else {
            sess->SetChanged(true);
            qint64 dif=sess->first()-stredf.startdate;
            int dn=dif/86400000L;
            if (dn<days) {
                int mode;
                sig=stredf.lookupSignal(CPAP_Mode);
                if (sig) {
                    mode=sig->data[dn];
                } else mode=0;

                sess->settings[CPAP_PresReliefType]=PR_EPR;

                // AutoSV machines don't have both fields
                sig=stredf.lookupSignal(RMS9_EPR);
                if (sig) {
                    int i=sig->data[dn];
                    sess->settings[CPAP_PresReliefMode]=i;

                }

                sig=stredf.lookupSignal(RMS9_EPRSet);
                if (sig)  {
                    sess->settings[CPAP_PresReliefSet]=sig->data[dn];
                }


                if (mode==0) {
                    sess->settings[CPAP_Mode]=MODE_CPAP;
                    sig=stredf.lookupSignal(RMS9_SetPressure); // ?? What's meant by Set Pressure?
                    if (sig) {
                        EventDataType pressure=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_Pressure]=pressure;
                    }
                } else if (mode>5) {
                    if (mode>=7)
                        sess->settings[CPAP_Mode]=MODE_ASV;
                    else
                        sess->settings[CPAP_Mode]=MODE_BIPAP;

                    EventDataType tmp,epap=0,ipap=0;
                    if (stredf.lookup.contains("EPAP")) {
                        sig=stredf.lookup["EPAP"];
                        epap=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_EPAP]=epap;
                    }
                    if (stredf.lookup.contains("IPAP")) {
                        sig=stredf.lookup["IPAP"];
                        ipap=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_IPAP]=ipap;
                    }
                    if (stredf.lookup.contains("PS")) {
                        sig=stredf.lookup["PS"];
                        tmp=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PS]=tmp; // technically this is IPAP-EPAP
                        if (!ipap) {
                            // not really possible. but anyway, just in case..
                            sess->settings[CPAP_IPAP]=epap+tmp;
                        }
                    }
                    if (stredf.lookup.contains("Min PS")) {
                        sig=stredf.lookup["Min PS"];
                        tmp=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PSMin]=tmp;
                        sess->settings[CPAP_IPAPLo]=epap+tmp;
                        sess->setMin(CPAP_IPAP,epap+tmp);
                    }
                    if (stredf.lookup.contains("Max PS")) {
                        sig=stredf.lookup["Max PS"];
                        tmp=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PSMax]=tmp;
                        sess->settings[CPAP_IPAPHi]=epap+tmp;
                    }
                    if (stredf.lookup.contains("RR")) { // Is this a setting to force respiratory rate on S/T machines?
                        sig=stredf.lookup["RR"];
                        tmp=sig->data[dn];
                        sess->settings[CPAP_RespRate]=tmp*sig->gain;
                    }

                    if (stredf.lookup.contains("Easy-Breathe")) {
                        sig=stredf.lookup["Easy-Breathe"];
                        tmp=sig->data[dn]*sig->gain;

                        sess->settings[CPAP_PresReliefSet]=tmp;
                        sess->settings[CPAP_PresReliefType]=(int)PR_EASYBREATHE;
                        sess->settings[CPAP_PresReliefMode]=(int)PM_FullTime;
                    }

                } else {
                    sess->settings[CPAP_Mode]=MODE_APAP;
                    sig=stredf.lookupSignal(CPAP_PressureMin);
                    if (sig) {
                        EventDataType pressure=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PressureMin]=pressure;
                        sess->setMin(CPAP_Pressure,pressure);
                    }
                    sig=stredf.lookupSignal(CPAP_PressureMax);
                    if (sig) {
                        EventDataType pressure=sig->data[dn]*sig->gain;
                        sess->settings[CPAP_PressureMax]=pressure;
                        sess->setMax(CPAP_Pressure,pressure);
                    }

                }
            }


            // The following only happens when the STR.edf file is not up to date..
            // This will only happen when the user fails to back up their SDcard properly.
            // Basically takes a guess.
            if (!sess->settings.contains(CPAP_Mode)) {
                //The following is a lame assumption if 50th percentile == max, then it's CPAP
                EventDataType p50=sess->percentile(CPAP_Pressure,0.50);
                EventDataType max=sess->Max(CPAP_Pressure);
                if (max==p50) {
                    sess->settings[CPAP_Mode]=MODE_CPAP;
                    sess->settings[CPAP_PressureMin]=p50;
                } else {
                    // It's not cpap, so just take the highest setting for this machines history.
                    // This may fail if the missing str data is at the beginning of a fresh import.
                    CPAPMode mode=(CPAPMode)(int)PROFILE.calcSettingsMax(CPAP_Mode,MT_CPAP,sess->machine()->FirstDay(),sess->machine()->LastDay());
                    if (mode<MODE_APAP) mode=MODE_APAP;
                    sess->settings[CPAP_Mode]=mode;
                    // Assuming 10th percentile should cover for ramp/warmup
                    sess->settings[CPAP_PressureMin]=sess->percentile(CPAP_Pressure,0.10);
                    sess->settings[CPAP_PressureMax]=sess->Max(CPAP_Pressure);
                }
            }
            //Rather than take a dodgy guess, EPR settings can take a hit, and this data can simply be missed..

            // Add the session to the machine & profile objects
            m->AddSession(sess,profile);
        }
    }


    if (m) {
        m->Save();
    }
    if (qprogress) qprogress->setValue(100);
    qDebug() << "Total Events " << event_cnt;
    return 1;
}

bool ResmedLoader::LoadEVE(Session *sess,EDFParser &edf)
{
    // EVEnt records have useless duration record.

    QString t;
    long recs;
    double duration;
    char * data;
    char c;
    long pos;
    bool sign,ok;
    double d;
    double tt;
    //ChannelID code;
    //Event *e;
    //totaldur=edf.GetNumDataRecords()*edf.GetDuration();

    EventList *EL[4]={NULL};
    sess->updateFirst(edf.startdate);
    //if (edf.enddate>edf.startdate) sess->set_last(edf.enddate);
    for (int s=0;s<edf.GetNumSignals();s++) {
        recs=edf.edfsignals[s]->nr*edf.GetNumDataRecords()*2;

        //qDebug() << edf.edfsignals[s]->label << " " << t;
        data=(char *)edf.edfsignals[s]->data;
        pos=0;
        tt=edf.startdate;
        sess->updateFirst(tt);
        duration=0;
        while (pos<recs) {
            c=data[pos];
            if ((c!='+') && (c!='-'))
                break;
            if (data[pos++]=='+') sign=true; else sign=false;
            t="";
            c=data[pos];
            do {
                t+=c;
                pos++;
                c=data[pos];
            } while ((c!=20) && (c!=21)); // start code
            d=t.toDouble(&ok);
            if (!ok) {
                qDebug() << "Faulty EDF EVE file " << edf.filename;
                break;
            }
            if (!sign) d=-d;
            tt=edf.startdate+qint64(d*1000.0);
            duration=0;
            // First entry

            if (data[pos]==21) {
                pos++;
                // get duration.
                t="";
                do {
                    t+=data[pos];
                    pos++;
                } while ((data[pos]!=20) && (pos<recs)); // start code
                duration=t.toDouble(&ok);
                if (!ok) {
                    qDebug() << "Faulty EDF EVE file (at %" << pos << ") " << edf.filename;
                    break;
                }
            }
            while ((data[pos]==20) && (pos<recs)) {
                t="";
                pos++;
                if (data[pos]==0)
                    break;
                if (data[pos]==20) {
                    pos++;
                    break;
                }

                do {
                    t+=tolower(data[pos++]);
                } while ((data[pos]!=20) && (pos<recs)); // start code
                if (!t.isEmpty()) {
                    if (t=="obstructive apnea") {
                        if (!EL[0]) {
                            if (!(EL[0]=sess->AddEventList(CPAP_Obstructive,EVL_Event))) return false;
                        }
                        EL[0]->AddEvent(tt,duration);
                    } else if (t=="hypopnea") {
                        if (!EL[1]) {
                            if (!(EL[1]=sess->AddEventList(CPAP_Hypopnea,EVL_Event))) return false;
                        }
                        EL[1]->AddEvent(tt,duration+10); // Only Hyponea's Need the extra duration???
                    } else if (t=="apnea") {
                        if (!EL[2]) {
                            if (!(EL[2]=sess->AddEventList(CPAP_Apnea,EVL_Event))) return false;
                        }
                        EL[2]->AddEvent(tt,duration);
                    } else if (t=="central apnea") {
                        //code=CPAP_ClearAirway;
                        if (!EL[3]) {
                            if (!(EL[3]=sess->AddEventList(CPAP_ClearAirway,EVL_Event))) return false;
                        }
                        EL[3]->AddEvent(tt,duration);
                    } else {
                        if (t!="recording starts") {
                            qDebug() << "Unobserved ResMed annotation field: " << t;
                        }
                    }
                }
                if (pos>=recs) {
                    qDebug() << "Short EDF EVE file" << edf.filename;
                    break;
                }
               // pos++;
            }
            while ((data[pos]==0) && pos<recs) pos++;
            if (pos>=recs) break;
        }
        sess->updateLast(tt);
       // qDebug(data);
    }
    return true;
}
bool ResmedLoader::LoadBRP(Session *sess,EDFParser &edf)
{
    QString t;
    sess->updateFirst(edf.startdate);
    qint64 duration=edf.GetNumDataRecords()*edf.GetDuration();
    sess->updateLast(edf.startdate+duration);

    for (int s=0;s<edf.GetNumSignals();s++) {
        EDFSignal & es=*edf.edfsignals[s];
        //qDebug() << "BRP:" << es.digital_maximum << es.digital_minimum << es.physical_maximum << es.physical_minimum;
        long recs=edf.edfsignals[s]->nr*edf.GetNumDataRecords();
        ChannelID code;
        if (edf.edfsignals[s]->label=="Flow") {
            es.gain*=60;
            es.physical_dimension="L/M";
            code=CPAP_FlowRate;
        } else if (edf.edfsignals[s]->label.startsWith("Mask Pres")) {
            code=CPAP_MaskPressureHi;
        } else if (es.label.startsWith("Resp Event")) {
            code=CPAP_RespEvent;
        } else {
            qDebug() << "Unobserved ResMed BRP Signal " << edf.edfsignals[s]->label;
            continue;
        }
        double rate=double(duration)/double(recs);
        EventList *a=sess->AddEventList(code,EVL_Waveform,es.gain,es.offset,0,0,rate);
        a->setDimension(es.physical_dimension);
        a->AddWaveform(edf.startdate,es.data,recs,duration);
        sess->setMin(code,a->Min());
        sess->setMax(code,a->Max());
        //delete edf.edfsignals[s]->data;
        //edf.edfsignals[s]->data=NULL; // so it doesn't get deleted when edf gets trashed.
    }
    return true;
}
EventList * ResmedLoader::ToTimeDelta(Session *sess,EDFParser &edf, EDFSignal & es, ChannelID code, long recs, qint64 duration,EventDataType min,EventDataType max,bool square)
{
    bool first=true;
    double rate=(duration/recs); // milliseconds per record
    double tt=edf.startdate;
    //sess->UpdateFirst(tt);
    EventDataType c,last;

    EventList *el=sess->AddEventList(code,EVL_Event,es.gain,es.offset,min,max);
    int startpos=0;

    if ((code==CPAP_Pressure) || (code==CPAP_IPAP) || (code==CPAP_EPAP)) {
        startpos=20; // Shave the first 20 seconds of pressure data
        tt+=rate*startpos;
    }
    for (int i=startpos;i<recs;i++) {
        c=es.data[i];

        if (first) {
            el->AddEvent(tt,c);
            first=false;
        } else {
            if (last!=c) {
                if (square) el->AddEvent(tt,last); // square waves look better on some charts.
                el->AddEvent(tt,c);
            }
        }
        tt+=rate;

        last=c;
    }
    el->AddEvent(tt,c);
    sess->updateLast(tt);
    return el;
}
bool ResmedLoader::LoadSAD(Session *sess,EDFParser &edf)
{
    QString t;
    sess->updateFirst(edf.startdate);
    qint64 duration=edf.GetNumDataRecords()*edf.GetDuration();
    sess->updateLast(edf.startdate+duration);

    for (int s=0;s<edf.GetNumSignals();s++) {
        EDFSignal & es=*edf.edfsignals[s];
        //qDebug() << "SAD:" << es.label << es.digital_maximum << es.digital_minimum << es.physical_maximum << es.physical_minimum;
        long recs=edf.edfsignals[s]->nr*edf.GetNumDataRecords();
        ChannelID code;
        if (edf.edfsignals[s]->label.startsWith("Puls")) {
            code=OXI_Pulse;
        } else if (edf.edfsignals[s]->label=="SpO2") {
            code=OXI_SPO2;
        } else {
            qDebug() << "Unobserved ResMed SAD Signal " << edf.edfsignals[s]->label;
            continue;
        }
        bool hasdata=false;
        for (int i=0;i<recs;i++) {
            if (es.data[i]!=-1) {
                hasdata=true;
                break;
            }
        }
        if (hasdata) {
            EventList *a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
            if (a) {
                sess->setMin(code,a->Min());
                sess->setMax(code,a->Max());
            }
        }

    }
    return true;
}


bool ResmedLoader::LoadPLD(Session *sess,EDFParser &edf)
{
    // Is it save to assume the order does not change here?
    enum PLDType { MaskPres=0, TherapyPres, ExpPress, Leak, RR, Vt, Mv, SnoreIndex, FFLIndex, U1, U2 };

    qint64 duration=edf.GetNumDataRecords()*edf.GetDuration();
    sess->updateFirst(edf.startdate);
    sess->updateLast(edf.startdate+duration);
    QString t;
    int emptycnt=0;
    EventList *a;
    double rate;
    long recs;
    ChannelID code;
    for (int s=0;s<edf.GetNumSignals();s++) {
        EDFSignal & es=*edf.edfsignals[s];
        recs=es.nr*edf.GetNumDataRecords();
        if (recs<=0) continue;
        rate=double(duration)/double(recs);
        //qDebug() << "EVE:" << es.digital_maximum << es.digital_minimum << es.physical_maximum << es.physical_minimum << es.gain;
        if (es.label=="Snore Index") {
            code=CPAP_Snore;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("Therapy Pres")) {
            code=CPAP_Pressure; //TherapyPressure;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label=="Insp Pressure") {
            code=CPAP_IPAP; //TherapyPressure;
            sess->settings[CPAP_Mode]=MODE_BIPAP;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if ((es.label=="MV") || (es.label=="VM")){
            code=CPAP_MinuteVent;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if ((es.label=="RR") || (es.label=="AF") || (es.label=="FR")) {
            code=CPAP_RespRate;
            a=sess->AddEventList(code,EVL_Waveform,es.gain,es.offset,0,0,rate);
            a->AddWaveform(edf.startdate,es.data,recs,duration);
        } else if ((es.label=="Vt") || (es.label=="VC")) {
            code=CPAP_TidalVolume;
            es.physical_maximum=es.physical_minimum=0;
            es.gain*=1000.0;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if ((es.label=="Leak") || (es.label.startsWith("Leck"))) {
            code=CPAP_Leak;
            es.gain*=60;
            es.physical_dimension="L/M";
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0,true);
        } else if (es.label=="FFL Index") {
            code=CPAP_FLG;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("Mask Pres")) {
            code=CPAP_MaskPressure;
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("Exp Press")) {
            code=CPAP_EPAP;//ExpiratoryPressure
            a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("I:E")) {
            code=CPAP_IE;//I:E ratio?
            a=sess->AddEventList(code,EVL_Waveform,es.gain,es.offset,0,0,rate);
            a->AddWaveform(edf.startdate,es.data,recs,duration);
            //a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("Ti")) {
            code=CPAP_Ti;
            a=sess->AddEventList(code,EVL_Waveform,es.gain,es.offset,0,0,rate);
            a->AddWaveform(edf.startdate,es.data,recs,duration);
            //a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("Te")) {
            code=CPAP_Te;
            a=sess->AddEventList(code,EVL_Waveform,es.gain,es.offset,0,0,rate);
            a->AddWaveform(edf.startdate,es.data,recs,duration);
            //a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label.startsWith("TgMV")) {
            code=CPAP_TgMV;
            a=sess->AddEventList(code,EVL_Waveform,es.gain,es.offset,0,0,rate);
            a->AddWaveform(edf.startdate,es.data,recs,duration);
            //a=ToTimeDelta(sess,edf,es, code,recs,duration,0,0);
        } else if (es.label=="") {
            if (emptycnt==0) {
                code=RMS9_E01;
                a=ToTimeDelta(sess,edf,es, code,recs,duration);
            } else if (emptycnt==1) {
                code=RMS9_E02;
                a=ToTimeDelta(sess,edf,es, code,recs,duration);
            } else {
                qDebug() << "Unobserved Empty Signal " << es.label;
            }
            emptycnt++;
        } else {
            qDebug() << "Unobserved ResMed PLD Signal " << es.label;
            a=NULL;
        }
        if (a) {
            sess->setMin(code,a->Min());
            sess->setMax(code,a->Max());
            a->setDimension(es.physical_dimension);
        }
    }
    return true;
}

void ResInitModelMap()
{
    // Courtesy Troy Schultz
    RMS9ModelMap[36001]="S9 Escape";
    RMS9ModelMap[36002]="S9 Escape Auto";
    RMS9ModelMap[36003]="S9 Elite";
    RMS9ModelMap[36004]="S9 VPAP S";
    RMS9ModelMap[36005]="S9 AutoSet";
    RMS9ModelMap[36006]="S9 VPAP Auto";
    RMS9ModelMap[36007]="S9 VPAP Adapt";
    RMS9ModelMap[36008]="S9 VPAP ST";
    /* S8 Series
    RMS9ModelMap[33007]="S8 Escape";
    RMS9ModelMap[33039]="S8 Elite II";
    RMS9ModelMap[33051]="S8 Escape II";
    RMS9ModelMap[33064]="S8 Escape II AutoSet";
    RMS9ModelMap[33064]="S8 Escape II AutoSet";
    RMS9ModelMap[33129]="S8 AutoSet II";
    */

    resmed_codes[CPAP_FlowRate].push_back("Flow");
    resmed_codes[CPAP_MaskPressureHi].push_back("Mask Pres");
    resmed_codes[CPAP_MaskPressureHi].push_back("Mask Pressure"); // vpap
    resmed_codes[CPAP_RespEvent].push_back("Resp Event");

    resmed_codes[CPAP_MaskPressure].push_back("Mask Pres");
    resmed_codes[CPAP_MaskPressure].push_back("Mask Pressure"); // vpap

    resmed_codes[CPAP_Pressure].push_back("Therapy Pres"); // not on vpap
    resmed_codes[CPAP_IPAP].push_back("Insp Pressure"); // on vpap

    resmed_codes[CPAP_EPAP].push_back("Exp Press");
    resmed_codes[CPAP_EPAP].push_back("Exp Pressure"); // vpap

    resmed_codes[CPAP_Leak].push_back("Leak");
    resmed_codes[CPAP_Leak].push_back("Leck.");

    resmed_codes[CPAP_RespRate].push_back("RR");
    resmed_codes[CPAP_RespRate].push_back("AF");
    resmed_codes[CPAP_RespRate].push_back("FR");

    resmed_codes[CPAP_TidalVolume].push_back("Vt");
    resmed_codes[CPAP_TidalVolume].push_back("VC");

    resmed_codes[CPAP_MinuteVent].push_back("MV");
    resmed_codes[CPAP_MinuteVent].push_back("VM");

    resmed_codes[CPAP_IE].push_back("I:E"); // vpap
    resmed_codes[CPAP_Snore].push_back("Snore Index");
    resmed_codes[CPAP_FLG].push_back("FFL Index");

    resmed_codes[CPAP_RespEvent].push_back("RE");
    resmed_codes[CPAP_Ti].push_back("Ti");
    resmed_codes[CPAP_Te].push_back("Te");

    // Sad (oximetry)
    resmed_codes[OXI_Pulse].push_back("Pulse");
    resmed_codes[OXI_Pulse].push_back("Puls");
    resmed_codes[OXI_SPO2].push_back("SpO2");

    // Event annotations
    resmed_codes[CPAP_Obstructive].push_back("Obstructive apnea");
    resmed_codes[CPAP_Hypopnea].push_back("Hypopnea");
    resmed_codes[CPAP_Apnea].push_back("Apnea");
    resmed_codes[CPAP_ClearAirway].push_back("Central apnea");

    resmed_codes[CPAP_Mode].push_back("Mode");
    resmed_codes[CPAP_Mode].push_back("Modus");
    resmed_codes[RMS9_SetPressure].push_back("Eingest. Druck");
    resmed_codes[RMS9_SetPressure].push_back("Set Pressure"); // Prescription
    resmed_codes[RMS9_SetPressure].push_back("Pres. prescrite");
    resmed_codes[RMS9_EPR].push_back("EPR");
    resmed_codes[RMS9_EPRSet].push_back("EPR Level");
    resmed_codes[RMS9_EPRSet].push_back("EPR-Stufe");
    resmed_codes[RMS9_EPRSet].push_back("Niveau EPR");
    resmed_codes[CPAP_PressureMax].push_back("Max Pressure");
    resmed_codes[CPAP_PressureMax].push_back("Max. Druck");
    resmed_codes[CPAP_PressureMax].push_back("Pression max.");

    resmed_codes[CPAP_PressureMin].push_back("Min Pressure");
    resmed_codes[CPAP_PressureMin].push_back("Min. Druck");
    resmed_codes[CPAP_PressureMin].push_back("Pression min.");

    // STR.edf
}


bool resmed_initialized=false;
void ResmedLoader::Register()
{
    if (resmed_initialized) return;
    qDebug() << "Registering ResmedLoader";
    RegisterLoader(new ResmedLoader());
    ResInitModelMap();
    resmed_initialized=true;
}

