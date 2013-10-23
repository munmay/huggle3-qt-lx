//This program is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.

//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.

#include "revertquery.h"

using namespace Huggle;

RevertQuery::RevertQuery()
{
    this->Type = QueryRevert;
    this->qRevert = NULL;
    this->edit = NULL;
    this->PreflightFinished = false;
    this->RollingBack = false;
    this->timer = NULL;
    this->UsingSR = false;
    this->Token = "";
    this->Summary = "";
    this->qPreflight = NULL;
}

RevertQuery::RevertQuery(WikiEdit *Edit)
{
    this->Type = QueryRevert;
    this->qRevert = NULL;
    this->edit = Edit;
    this->PreflightFinished = false;
    this->RollingBack = false;
    this->timer = NULL;
    this->IgnorePreflightCheck = false;
    this->UsingSR = false;
    this->Token = "";
    this->Summary = Configuration::GetDefaultRevertSummary(this->edit->User->Username);
    this->qPreflight = NULL;
}

void RevertQuery::Process()
{
    if (this->Status == StatusProcessing)
    {
        Core::DebugLog("Cowardly refusing to double process the query");
        return;
    }
    this->Status = StatusProcessing;
    if (timer != NULL)
    {
        delete timer;
    }
    this->timer = new QTimer(this);
    connect(this->timer, SIGNAL(timeout()), this, SLOT(OnTick()));
    this->timer->start(800);
    this->RegisterConsumer("RevertQuery::Timer");
    this->CustomStatus = "Preflight check";
    this->Preflight();
}

void RevertQuery::Kill()
{
    if (PreflightFinished && this->qRevert != NULL)
    {
        this->qRevert->Kill();
    } else if (this->qPreflight != NULL)
    {
        this->qPreflight->Kill();
    }
    this->Status = StatusInError;
    if (this->Result == NULL)
    {
        this->Result = new QueryResult();
        this->Result->ErrorMessage = "Killed";
        this->Result->Failed = true;
    }
    this->Exit();
}

RevertQuery::~RevertQuery()
{
    delete timer;
    if (this->qPreflight != NULL)
    {
        this->qPreflight->SafeDelete();
    }
    if (this->qRevert != NULL)
    {
        this->qRevert->SafeDelete();
    }
}

QString RevertQuery::QueryTargetToString()
{
    return this->edit->Page->PageName;
}

bool RevertQuery::Processed()
{
    if (!this->PreflightFinished)
    {
        return false;
    }

    if (this->Status != StatusDone)
    {
        if (CheckRevert())
        {
            this->Status = StatusDone;
            return true;
        }
        return false;
    }

    return true;
}

void RevertQuery::OnTick()
{
    if (this->Status != StatusDone)
    {
        if (!PreflightFinished)
        {
            CheckPreflight();
            return;
        }

        if (!RollingBack)
        {
            this->Rollback();
            return;
        }
    }

    if (Processed())
    {
        this->timer->stop();
        this->UnregisterConsumer("RevertQuery::Timer");
    }
}

QString RevertQuery::GetCustomRevertStatus(QString RevertData)
{
    QDomDocument d;
    d.setContent(RevertData);
    QDomNodeList l = d.elementsByTagName("error");
    if (l.count() > 0)
    {
        if (l.at(0).toElement().attributes().contains("code"))
        {
            QString Error = "";
            Error = l.at(0).toElement().attribute("code");

            if (Error == "alreadyrolled")
            {
                return "Edit was reverted by someone else - skipping";
            }

            if (Error == "onlyauthor")
            {
                return "ERROR: Cannot rollback - page only has one author";
            }
            return "In error (" + Error +")";
        }
    }
    return "Reverted";
}

void RevertQuery::Preflight()
{
    // check if there is more edits in queue
    int x=0;
    bool failed = false;
    bool MadeBySameUser = true;
    while (x < WikiEdit::EditList.count())
    {
        WikiEdit *w = WikiEdit::EditList.at(x);
        if (w != this->edit)
        {
            if (w->Page->PageName != this->edit->Page->PageName)
            {
                x++;
                continue;
            }
            if (w->Time > this->edit->Time)
            {
                if (w->User->Username != this->edit->User->Username)
                {
                    MadeBySameUser = false;
                }
                failed = true;
            }
        }
        x++;
    }
    if (failed)
    {
        if (Configuration::AutomaticallyResolveConflicts)
        {
            this->Cancel();
            return;
        }
        QString text;
        if (MadeBySameUser)
        {
            text = ("There are newer edits to " + this->edit->Page->PageName + ", are you sure you want to revert them?");
        } else
        {
            text = ("There are new edits made to " + this->edit->Page->PageName + " by a different user, are you sure you want to revert them all? (it will likely fail anyway because of old token)");
        }
        QMessageBox::StandardButton re;
        re = QMessageBox::question(Core::Main, "Preflight check", text, QMessageBox::Yes|QMessageBox::No);
        if (re == QMessageBox::No)
        {
            this->Cancel();
            return;
        } else
        {
            this->IgnorePreflightCheck = true;
        }
    }
    this->qPreflight = new ApiQuery();
    this->qPreflight->SetAction(ActionQuery);
    this->qPreflight->Parameters = "prop=revisions&rvprop=ids%7Cflags%7Ctimestamp%7Cuser%7Cuserid%7Csize%7Csha1%7Ccomment&rvlimit=20&titles="
                                    + QUrl::toPercentEncoding(this->edit->Page->PageName);
    this->qPreflight->Process();
}

void RevertQuery::CheckPreflight()
{
    if (this->IgnorePreflightCheck)
    {
        this->PreflightFinished = true;
        return;
    }
    if (this->qPreflight == NULL)
    {
        return;
    }
    if (!this->qPreflight->Processed())
    {
        return;
    }
    if (this->qPreflight->Result->Failed)
    {
        Core::Log("Failed to preflight check the edit: " + this->qPreflight->Result->ErrorMessage);
        this->Kill();
        this->Status = StatusDone;
        this->Result = new QueryResult();
        this->Result->Failed = true;
        return;
    }
    QDomDocument d;
    d.setContent(this->qPreflight->Result->Data);
    QDomNodeList l = d.elementsByTagName("rev");
    int x=0;
    bool MadeBySameUser = true;
    bool passed = true;
    while (x < l.count())
    {
        QDomElement e = l.at(x).toElement();
        if (e.attributes().contains("revid"))
        {
            if (edit->RevID == e.attribute("revid").toInt())
            {
                x++;
                continue;
            }
        } else
        {
            x++;
            continue;
        }
        if (this->edit->RevID != WIKI_UNKNOWN_REVID && e.attribute("revid").toInt() > edit->RevID)
        {
            passed = false;
        }
        x++;
    }

    if (!passed)
    {
        QString text = ":)";
        if (MadeBySameUser)
        {
            text = ("There are newer edits to " + this->edit->Page->PageName + ", are you sure you want to revert them");
        } else
        {
            text = ("There are new edits made to " + this->edit->Page->PageName + " by a different user, are you sure you want to revert them all? (it will likely fail anyway because of old token)");
        }
        if (Configuration::AutomaticallyResolveConflicts)
        {
            this->Cancel();
            return;
        }
        QMessageBox::StandardButton re;
        re = QMessageBox::question(Core::Main, "Preflight check", text, QMessageBox::Yes|QMessageBox::No);
        if (re == QMessageBox::No)
        {
            // abort
            this->Exit();
            this->CustomStatus = "Stopped";
            this->Result = new QueryResult();
            this->Result->Failed = true;
            this->Result->ErrorMessage = "User requested to abort this";
            this->Status = StatusDone;
            this->PreflightFinished = true;
            return;
        }
    }

    this->PreflightFinished = true;
}

bool RevertQuery::CheckRevert()
{
    if (this->UsingSR)
    {
        return false;
    }
    if (this->qRevert == NULL)
    {
        return false;
    }
    if (!this->qRevert->Processed())
    {
        return false;
    }
    this->CustomStatus = RevertQuery::GetCustomRevertStatus(this->qRevert->Result->Data);
    if (this->CustomStatus != "Reverted")
    {
        Core::Log("Unable to revert " + this->qRevert->Target + ": " + this->CustomStatus);
        qRevert->Result->Failed = true;
        qRevert->Result->ErrorMessage = CustomStatus;
        this->Result = new QueryResult();
        this->Result->ErrorMessage = CustomStatus;
        this->Result->Failed = true;
    } else
    {
        HistoryItem item;
        this->Result = new QueryResult();
        this->Result->Data = this->qRevert->Result->Data;
        item.Target = this->qRevert->Target;
        item.Type = HistoryRollback;
        item.Result = "Success";
        if (Core::Main != NULL)
        {
            Core::Main->_History->Prepend(item);
        }
    }
    this->qRevert->UnregisterConsumer("RevertQuery");
    this->qRevert = NULL;
    return true;
}

void RevertQuery::Cancel()
{
    this->Exit();
    this->CustomStatus = "Stopped";
    this->Result = new QueryResult();
    this->Result->Failed = true;
    this->Result->ErrorMessage = "User requested to abort this";
    this->Status = StatusDone;
    this->PreflightFinished = true;
}

void RevertQuery::Rollback()
{
    if (this->RollingBack)
    {
        // wtf happened
        Core::DebugLog("Multiple request to rollback same query");
        return;
    }
    this->RollingBack = true;

    if (this->Summary == "")
    {
        this->Summary = Configuration::GetDefaultRevertSummary(this->edit->User->Username);
    }

    if (this->Summary.contains("$1"))
    {
        this->Summary = this->Summary.replace("$1", edit->User->Username);
    }

    edit->User->BadnessScore += 200;
    WikiUser::UpdateUser(edit->User);

    if (this->UsingSR)
    {
        Revert();
        return;
    }
    if (!Configuration::Rights.contains("rollback"))
    {
        Core::Log("You don't have rollback rights, fallback to software rollback");
        this->UsingSR = true;
        this->Revert();
        return;
    }

    if (this->Token == "")
    {
        this->Token = this->edit->RollbackToken;
    }

    if (this->Token == "")
    {
        Core::Log("ERROR, unable to rollback, because the rollback token was empty: " + this->edit->Page->PageName);
        this->Result = new QueryResult();
        this->Result->Failed = true;
        this->Result->ErrorMessage = "ERROR, unable to rollback, because the rollback token was empty: " + this->edit->Page->PageName;
        this->Status = StatusDone;
        this->Exit();
        return;
    }
    this->qRevert = new ApiQuery();
    this->qRevert->SetAction(ActionRollback);
    QString token = this->Token;
    if (token.endsWith("+\\"))
    {
        token = QUrl::toPercentEncoding(token);
    }
    this->qRevert->Parameters = "title=" + QUrl::toPercentEncoding(edit->Page->PageName)
                + "&token=" + token
                + "&user=" + QUrl::toPercentEncoding(edit->User->Username)
                + "&summary=" + QUrl::toPercentEncoding(this->Summary);
    this->qRevert->Target = edit->Page->PageName;
    this->qRevert->UsingPOST = true;
    this->qRevert->RegisterConsumer("RevertQuery");
    if (Configuration::Verbosity > 0)
    {
        Core::AppendQuery(this->qRevert);
    }
    // TRANSLATE ME
    this->CustomStatus = "Rolling back " + edit->Page->PageName;
    Core::DebugLog("Rolling back " + edit->Page->PageName);
    this->qRevert->Process();
}

void RevertQuery::Revert()
{
    Core::Log("ERROR: this is not implemented yet");
}

void RevertQuery::Exit()
{
    if (this->timer != NULL)
    {
        this->timer->stop();
    }
    this->UnregisterConsumer("RevertQuery::Timer");
    if (this->qRevert != NULL)
    {
        this->qRevert->UnregisterConsumer("RevertQuery");
    }
}