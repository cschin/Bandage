﻿//Copyright 2015 Ryan Wick

//This file is part of Bandage

//Bandage is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.

//Bandage is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.

//You should have received a copy of the GNU General Public License
//along with Bandage.  If not, see <http://www.gnu.org/licenses/>.


#include "blastsearchdialog.h"
#include "ui_blastsearchdialog.h"

#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QString>
#include "../blast/blasthit.h"
#include "../blast/blastquery.h"
#include <QStandardItemModel>
#include "../program/globals.h"
#include "../program/settings.h"
#include "../graph/debruijnnode.h"
#include <QMessageBox>
#include <QDir>
#include "enteroneblastquerydialog.h"
#include "../graph/assemblygraph.h"
#include "../blast/blastsearch.h"
#include <QProcessEnvironment>
#include <QMessageBox>

BlastSearchDialog::BlastSearchDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::BlastSearchDialog), m_makeblastdbCommand("makeblastdb"), m_blastnCommand("blastn")
{
    ui->setupUi(this);

    //If a BLAST database already exists, move to step 2.
    QFile databaseFile(g_tempDirectory + "all_nodes.fasta");
    if (databaseFile.exists())
        setUiStep(2);

    //If there isn't a BLAST database, clear the entire temporary directory
    //and move to step 1.
    else
    {
        emptyTempDirectory();
        setUiStep(1);
    }

    //If queries already exist, display them and move to step 3.
    if (g_blastSearch->m_blastQueries.m_queries.size() > 0)
    {
        fillQueriesTable();
        setUiStep(3);
    }

    //If results already exist, display them and move to step 3.
    if (g_blastSearch->m_hits.size() > 0)
    {
        fillHitsTable();
        setUiStep(3);
    }

    ui->parametersLineEdit->setText(g_settings->blastSearchParameters);
    ui->timeoutSpinBox->setValue(g_settings->blastTimeoutSeconds);
    setInfoTexts();

    connect(ui->buildBlastDatabaseButton, SIGNAL(clicked()), this, SLOT(buildBlastDatabase1()));
    connect(ui->loadQueriesFromFastaButton, SIGNAL(clicked()), this, SLOT(loadBlastQueriesFromFastaFile()));
    connect(ui->enterQueryManuallyButton, SIGNAL(clicked()), this, SLOT(enterQueryManually()));
    connect(ui->clearQueriesButton, SIGNAL(clicked()), this, SLOT(clearQueries()));
    connect(ui->startBlastSearchButton, SIGNAL(clicked()), this, SLOT(runBlastSearch()));
}

BlastSearchDialog::~BlastSearchDialog()
{
    delete ui;
}


void BlastSearchDialog::clearBlastHits()
{
    g_blastSearch->m_hits.clear();
    g_blastSearch->m_blastQueries.clearSearchResults();
    ui->blastHitsTableView->setModel(0);
}

void BlastSearchDialog::loadBlastHits(QString blastHits)
{
    QStringList blastHitList = blastHits.split("\n", QString::SkipEmptyParts);

    if (blastHitList.size() == 0)
    {
        QMessageBox::information(this, "No hits", "No BLAST hits were found for the given queries and parameters.");
        return;
    }

    for (int i = 0; i < blastHitList.size(); ++i)
    {
        QString hit = blastHitList[i];
        QStringList alignmentParts = hit.split('\t');

        QString queryName = alignmentParts[0];
        QString nodeLabel = alignmentParts[1];
        int queryStart = alignmentParts[6].toInt();
        int queryEnd = alignmentParts[7].toInt();
        int nodeStart = alignmentParts[8].toInt();
        int nodeEnd = alignmentParts[9].toInt();
        QString eValue = alignmentParts[10];

        //Only save BLAST hits that are on forward strands.
        if (nodeStart > nodeEnd)
            continue;

        long long nodeNumber = getNodeNumberFromString(nodeLabel);
        DeBruijnNode * node;
        if (g_assemblyGraph->m_deBruijnGraphNodes.contains(nodeNumber))
            node = g_assemblyGraph->m_deBruijnGraphNodes[nodeNumber];
        else
            return;

        BlastQuery * query = g_blastSearch->m_blastQueries.getQueryFromName(queryName);
        if (query == 0)
            return;

        g_blastSearch->m_hits.push_back(BlastHit(node, nodeStart, nodeEnd,
                                                        query, queryStart, queryEnd, eValue));

        ++(query->m_hits);
    }

    fillQueriesTable();
    fillHitsTable();
}


long long BlastSearchDialog::getNodeNumberFromString(QString nodeString)
{
    QStringList nodeStringParts = nodeString.split("_");
    return nodeStringParts[1].toLongLong();
}



void BlastSearchDialog::fillQueriesTable()
{
    int queryCount = int(g_blastSearch->m_blastQueries.m_queries.size());
    if (queryCount == 0)
        return;

    QStandardItemModel * model = new QStandardItemModel(queryCount, 3, this); //3 Columns
    model->setHorizontalHeaderItem(0, new QStandardItem("Target name"));
    model->setHorizontalHeaderItem(1, new QStandardItem("Target length"));
    model->setHorizontalHeaderItem(2, new QStandardItem("Hits"));
    for (int i = 0; i < queryCount; ++i)
    {
        BlastQuery * query = &(g_blastSearch->m_blastQueries.m_queries[i]);
        model->setItem(i, 0, new QStandardItem(query->m_name));
        model->setItem(i, 1, new QStandardItem(formatIntForDisplay(query->m_length)));

        //If the search hasn't yet been run, don't put a number in the hits column.
        if (query->m_searchedFor)
            model->setItem(i, 2, new QStandardItem(formatIntForDisplay(query->m_hits)));
        else
            model->setItem(i, 2, new QStandardItem("-"));
    }
    ui->blastQueriesTableView->setModel(model);
    ui->blastQueriesTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    setUiStep(3);
}


void BlastSearchDialog::fillHitsTable()
{
    int hitCount = int(g_blastSearch->m_hits.size());

    QStandardItemModel * model = new QStandardItemModel(hitCount, 8, this); //8 Columns
    model->setHorizontalHeaderItem(0, new QStandardItem("Node number"));
    model->setHorizontalHeaderItem(1, new QStandardItem("Node length"));
    model->setHorizontalHeaderItem(2, new QStandardItem("Node start"));
    model->setHorizontalHeaderItem(3, new QStandardItem("Node end"));
    model->setHorizontalHeaderItem(4, new QStandardItem("Query name"));
    model->setHorizontalHeaderItem(5, new QStandardItem("Query start"));
    model->setHorizontalHeaderItem(6, new QStandardItem("Query end"));
    model->setHorizontalHeaderItem(7, new QStandardItem("E-value"));

    for (int i = 0; i < hitCount; ++i)
    {
        BlastHit * hit = &(g_blastSearch->m_hits[i]);
        model->setItem(i, 0, new QStandardItem(hit->m_node->getNodeNumberText(true)));
        model->setItem(i, 1, new QStandardItem(formatIntForDisplay(hit->m_node->m_length)));
        model->setItem(i, 2, new QStandardItem(formatIntForDisplay(hit->m_nodeStart)));
        model->setItem(i, 3, new QStandardItem(formatIntForDisplay(hit->m_nodeEnd)));
        model->setItem(i, 4, new QStandardItem(hit->m_query->m_name));
        model->setItem(i, 5, new QStandardItem(formatIntForDisplay(hit->m_queryStart)));
        model->setItem(i, 6, new QStandardItem(formatIntForDisplay(hit->m_queryEnd)));
        model->setItem(i, 7, new QStandardItem(hit->m_eValue));
    }
    ui->blastHitsTableView->setModel(model);
    ui->blastHitsTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    ui->blastHitsTableView->setEnabled(true);
}


void BlastSearchDialog::buildBlastDatabase1()
{
    QString findMakeblastdbCommand = "which makeblastdb";
#ifdef Q_OS_WIN32
    findMakeblastdbCommand = "WHERE makeblastdb";
#endif

    QProcess findMakeblastdb;

    //On Mac, it's necessary to do some stuff with the PATH variable in order
    //for which to work.
#ifdef Q_OS_MAC
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList envlist = env.toStringList();

    //Add some paths to the process environment
    envlist.replaceInStrings(QRegularExpression("^(?i)PATH=(.*)"), "PATH="
                                                                   "/usr/bin:"
                                                                   "/bin:"
                                                                   "/usr/sbin:"
                                                                   "/sbin:"
                                                                   "/opt/local/bin:"
                                                                   "/usr/local/bin:"
                                                                   "$HOME/bin:"
                                                                   "/usr/local/ncbi/blast/bin:"
                                                                   "\\1");

    findMakeblastdb.setEnvironment(envlist);
#endif

    findMakeblastdb.start(findMakeblastdbCommand);
    findMakeblastdb.waitForFinished();

    //On Mac, the command for makeblastdb needs to be the absolute path.
#ifdef Q_OS_MAC
    m_makeblastdbCommand = QString(findMakeblastdb.readAll()).simplified();
#endif

    if (findMakeblastdb.exitCode() != 0)
    {
        QMessageBox::warning(this, "Error", "The program makeblastdb was not found.  Please install NCBI BLAST to use this feature.");
        return;
    }

    ui->buildBlastDatabaseButton->setEnabled(false);
    ui->buildBlastDatabaseInfoText->setEnabled(false);

    QApplication::processEvents();

    emit createAllNodesFasta(g_tempDirectory, false, false);
}


void BlastSearchDialog::buildBlastDatabase2()
{
    QProcess makeblastdb;
    makeblastdb.start(m_makeblastdbCommand + " -in " + g_tempDirectory + "all_nodes.fasta " + "-dbtype nucl");


    g_settings->blastTimeoutSeconds = ui->timeoutSpinBox->value();
    bool finished = makeblastdb.waitForFinished(g_settings->blastTimeoutSeconds * 1000); //Multiply by 1000 because this function takes milliseconds

    if (makeblastdb.exitCode() != 0)
    {
        QMessageBox::warning(this, "Error", "There was a problem building the BLAST database.");
        setUiStep(1);
        return;
    }
    else if (!finished)
    {
        QMessageBox::warning(this, "Error", "The BLAST database did not build in the allotted time.\n\n"
                                            "Increase the 'Allowed time' setting and try again.");
        setUiStep(1);
        return;
    }

    setUiStep(2);
}


void BlastSearchDialog::loadBlastQueriesFromFastaFile()
{
    QString fullFileName = QFileDialog::getOpenFileName(this, "Load queries FASTA", g_settings->rememberedPath);

    if (fullFileName != "") //User did not hit cancel
    {
        std::vector<QString> queryNames;
        std::vector<QString> querySequences;
        readFastaFile(fullFileName, &queryNames, &querySequences);

        for (size_t i = 0; i < queryNames.size(); ++i)
        {
            QString queryName = cleanQueryName(queryNames[i]);
            g_blastSearch->m_blastQueries.addQuery(BlastQuery(queryName,
                                                              querySequences[i]));
        }

        fillQueriesTable();
        clearBlastHits();
        g_settings->rememberedPath = QFileInfo(fullFileName).absolutePath();
    }
}


QString BlastSearchDialog::cleanQueryName(QString queryName)
{
    //Replace whitespace with underscores
    queryName = queryName.replace(QRegExp("\\s"), "_");

    //Remove any dots from the end of the query name.  BLAST doesn't
    //include them in its results, so if we don't remove them, then
    //we won't be able to find a match between the query name and
    //the BLAST hit.
    while (queryName.length() > 0 && queryName[queryName.size() - 1] == '.')
        queryName = queryName.left(queryName.size() - 1);

    return queryName;
}


void BlastSearchDialog::enterQueryManually()
{
    EnterOneBlastQueryDialog enterOneBlastQueryDialog(this);

    if (enterOneBlastQueryDialog.exec())
    {
        QString queryName = cleanQueryName(enterOneBlastQueryDialog.getName());
        g_blastSearch->m_blastQueries.addQuery(BlastQuery(queryName,
                                                          enterOneBlastQueryDialog.getSequence()));
        fillQueriesTable();
        clearBlastHits();
    }
}



void BlastSearchDialog::clearQueries()
{
    g_blastSearch->m_blastQueries.clearQueries();
    ui->blastQueriesTableView->setModel(0);
    ui->clearQueriesButton->setEnabled(false);

    clearBlastHits();
    setUiStep(2);
}



void BlastSearchDialog::runBlastSearch()
{    
    QString findBlastnCommand = "which blastn";
#ifdef Q_OS_WIN32
    findBlastnCommand = "WHERE blastn";
#endif

    QProcess findBlastn;

    //On Mac, it's necessary to do some stuff with the PATH variable in order
    //for which to work.
#ifdef Q_OS_MAC
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList envlist = env.toStringList();

    //Add some paths to the process environment
    envlist.replaceInStrings(QRegularExpression("^(?i)PATH=(.*)"), "PATH="
                                                                   "/usr/bin:"
                                                                   "/bin:"
                                                                   "/usr/sbin:"
                                                                   "/sbin:"
                                                                   "/opt/local/bin:"
                                                                   "/usr/local/bin:"
                                                                   "$HOME/bin:"
                                                                   "/usr/local/ncbi/blast/bin:"
                                                                   "\\1");

    findBlastn.setEnvironment(envlist);
#endif

    findBlastn.start(findBlastnCommand);
    findBlastn.waitForFinished();

    //On Mac, the command for makeblastdb needs to be the absolute path.
#ifdef Q_OS_MAC
    m_blastnCommand = QString(findBlastn.readAll()).simplified();
#endif

    if (findBlastn.exitCode() != 0)
    {
        QMessageBox::warning(this, "Error", "The program blastn was not found.  Please install NCBI BLAST to use this feature.");
        return;
    }

    ui->startBlastSearchButton->setEnabled(false);
    ui->parametersInfoText->setEnabled(false);
    ui->parametersLineEdit->setEnabled(false);
    ui->parametersLabel->setEnabled(false);
    ui->timeoutInfoText->setEnabled(false);
    ui->timeoutSpinBox->setEnabled(false);
    ui->timeoutLabel->setEnabled(false);


    QApplication::processEvents();

    QProcess blastn;
    QString extraCommandLineOptions = ui->parametersLineEdit->text().simplified() + " ";
    QString fullBlastnCommand = m_blastnCommand + " -query " + g_tempDirectory + "queries.fasta -db " + g_tempDirectory + "all_nodes.fasta -outfmt 6 " + extraCommandLineOptions;

    blastn.start(fullBlastnCommand);

    g_settings->blastTimeoutSeconds = ui->timeoutSpinBox->value();
    bool finished = blastn.waitForFinished(g_settings->blastTimeoutSeconds * 1000); //Multiply by 1000 because this function takes milliseconds

    QString blastHits = blastn.readAll();

    if (blastn.exitCode() != 0)
    {
        QMessageBox::warning(this, "Error", "There was a problem running the BLAST search.");

        ui->startBlastSearchButton->setEnabled(true);
        ui->parametersInfoText->setEnabled(true);
        ui->parametersLineEdit->setEnabled(true);
        ui->parametersLabel->setEnabled(true);
        ui->timeoutInfoText->setEnabled(true);
        ui->timeoutSpinBox->setEnabled(true);
        ui->timeoutLabel->setEnabled(true);

        return;
    }
    else if (!finished)
    {
        QMessageBox::warning(this, "Error", "The BLAST search did not finish in the allotted time.\n\n"
                                            "Increase the 'Allowed time' setting and try again.");

        ui->startBlastSearchButton->setEnabled(true);
        ui->parametersInfoText->setEnabled(true);
        ui->parametersLineEdit->setEnabled(true);
        ui->parametersLabel->setEnabled(true);
        ui->timeoutInfoText->setEnabled(true);
        ui->timeoutSpinBox->setEnabled(true);
        ui->timeoutLabel->setEnabled(true);

        return;
    }

    clearBlastHits();
    g_blastSearch->m_blastQueries.searchOccurred();
    loadBlastHits(blastHits);
    g_settings->blastSearchParameters = extraCommandLineOptions;
    setUiStep(4);
}



void BlastSearchDialog::setUiStep(int step)
{
    QPixmap tick(":/icons/tick-128.png");
    QPixmap tickScaled = tick.scaled(32, 32);

    switch (step)
    {
    //Step 1 is for when the BLAST database has not yet been made.
    case 1:
        ui->step1Label->setEnabled(true);
        ui->buildBlastDatabaseButton->setEnabled(true);
        ui->timeoutLabel->setEnabled(true);
        ui->timeoutSpinBox->setEnabled(true);
        ui->timeoutInfoText->setEnabled(true);
        ui->step2Label->setEnabled(false);
        ui->loadQueriesFromFastaButton->setEnabled(false);
        ui->enterQueryManuallyButton->setEnabled(false);
        ui->blastQueriesTableView->setEnabled(false);
        ui->step3Label->setEnabled(false);
        ui->parametersLabel->setEnabled(false);
        ui->parametersLineEdit->setEnabled(false);
        ui->startBlastSearchButton->setEnabled(false);
        ui->clearQueriesButton->setEnabled(false);
        ui->hitsLabel->setEnabled(false);
        ui->step1TickLabel->setPixmap(QPixmap());
        ui->step2TickLabel->setPixmap(QPixmap());
        ui->step3TickLabel->setPixmap(QPixmap());
        ui->buildBlastDatabaseInfoText->setEnabled(true);
        ui->loadQueriesFromFastaInfoText->setEnabled(false);
        ui->enterQueryManuallyInfoText->setEnabled(false);
        ui->parametersInfoText->setEnabled(false);
        ui->startBlastSearchInfoText->setEnabled(false);
        ui->clearQueriesInfoText->setEnabled(false);
        break;

    //Step 2 is for loading queries
    case 2:
        ui->step1Label->setEnabled(true);
        ui->buildBlastDatabaseButton->setEnabled(false);
        ui->timeoutLabel->setEnabled(true);
        ui->timeoutSpinBox->setEnabled(true);
        ui->timeoutInfoText->setEnabled(true);
        ui->step2Label->setEnabled(true);
        ui->loadQueriesFromFastaButton->setEnabled(true);
        ui->enterQueryManuallyButton->setEnabled(true);
        ui->blastQueriesTableView->setEnabled(true);
        ui->step3Label->setEnabled(false);
        ui->parametersLabel->setEnabled(false);
        ui->parametersLineEdit->setEnabled(false);
        ui->startBlastSearchButton->setEnabled(false);
        ui->clearQueriesButton->setEnabled(false);
        ui->hitsLabel->setEnabled(false);
        ui->step1TickLabel->setPixmap(tickScaled);
        ui->step2TickLabel->setPixmap(QPixmap());
        ui->step3TickLabel->setPixmap(QPixmap());
        ui->buildBlastDatabaseInfoText->setEnabled(false);
        ui->loadQueriesFromFastaInfoText->setEnabled(true);
        ui->enterQueryManuallyInfoText->setEnabled(true);
        ui->parametersInfoText->setEnabled(false);
        ui->startBlastSearchInfoText->setEnabled(false);
        ui->clearQueriesInfoText->setEnabled(false);
        break;

    //Step 3 is for running the BLAST search
    case 3:
        ui->step1Label->setEnabled(true);
        ui->buildBlastDatabaseButton->setEnabled(false);
        ui->timeoutLabel->setEnabled(true);
        ui->timeoutSpinBox->setEnabled(true);
        ui->timeoutInfoText->setEnabled(true);
        ui->step2Label->setEnabled(true);
        ui->loadQueriesFromFastaButton->setEnabled(true);
        ui->enterQueryManuallyButton->setEnabled(true);
        ui->blastQueriesTableView->setEnabled(true);
        ui->step3Label->setEnabled(true);
        ui->parametersLabel->setEnabled(true);
        ui->parametersLineEdit->setEnabled(true);
        ui->startBlastSearchButton->setEnabled(true);
        ui->clearQueriesButton->setEnabled(true);
        ui->hitsLabel->setEnabled(false);
        ui->step1TickLabel->setPixmap(tickScaled);
        ui->step2TickLabel->setPixmap(tickScaled);
        ui->step3TickLabel->setPixmap(QPixmap());
        ui->buildBlastDatabaseInfoText->setEnabled(false);
        ui->loadQueriesFromFastaInfoText->setEnabled(true);
        ui->enterQueryManuallyInfoText->setEnabled(true);
        ui->parametersInfoText->setEnabled(true);
        ui->startBlastSearchInfoText->setEnabled(true);
        ui->clearQueriesInfoText->setEnabled(true);
        break;

    //Step 4 is after the BLAST search has been run.
    case 4:
        ui->step1Label->setEnabled(true);
        ui->buildBlastDatabaseButton->setEnabled(false);
        ui->timeoutLabel->setEnabled(true);
        ui->timeoutSpinBox->setEnabled(true);
        ui->timeoutInfoText->setEnabled(true);
        ui->step2Label->setEnabled(true);
        ui->loadQueriesFromFastaButton->setEnabled(true);
        ui->enterQueryManuallyButton->setEnabled(true);
        ui->blastQueriesTableView->setEnabled(true);
        ui->step3Label->setEnabled(true);
        ui->parametersLabel->setEnabled(true);
        ui->parametersLineEdit->setEnabled(true);
        ui->startBlastSearchButton->setEnabled(true);
        ui->clearQueriesButton->setEnabled(true);
        ui->hitsLabel->setEnabled(true);
        ui->step1TickLabel->setPixmap(tickScaled);
        ui->step2TickLabel->setPixmap(tickScaled);
        ui->step3TickLabel->setPixmap(tickScaled);
        ui->buildBlastDatabaseInfoText->setEnabled(false);
        ui->loadQueriesFromFastaInfoText->setEnabled(true);
        ui->enterQueryManuallyInfoText->setEnabled(true);
        ui->parametersInfoText->setEnabled(true);
        ui->startBlastSearchInfoText->setEnabled(true);
        ui->clearQueriesInfoText->setEnabled(true);
        break;
    }
}



void BlastSearchDialog::setInfoTexts()
{
    ui->buildBlastDatabaseInfoText->setInfoText("This step runs makeblastdb on the contig sequences, "
                                                "preparing them for a BLAST search.<br><br>"
                                                "The database files generated are temporary and will "
                                                "be deleted when Bandage is closed.");
    ui->timeoutInfoText->setInfoText("This is the number of seconds Bandage will wait for the BLAST database "
                                     "to build and for the BLAST search to complete. If the processes do not "
                                     "finish in this time, they will be halted.");
    ui->loadQueriesFromFastaInfoText->setInfoText("Click this button to load a FASTA file.  Each "
                                                  "sequence in the FASTA file will be a separate "
                                                  "query.");
    ui->enterQueryManuallyInfoText->setInfoText("Click this button to type or paste a single query sequence.");
    ui->parametersInfoText->setInfoText("You may add additional blastn parameters here, exactly as they "
                                        "would be typed at the command line.");
    ui->startBlastSearchInfoText->setInfoText("Click this to conduct a blastn search for the above "
                                              "queries on the graph nodes.<br><br>"
                                              "If no parameters were added above, this will run:<br>"
                                              "blastn -query queries.fasta -db all_nodes.fasta -outfmt 6<br><br>"
                                              "If, for example, '-evalue 0.01' was entered in the above "
                                              "parameters field, then this will run:<br>"
                                              "blastn -query queries.fasta -db all_nodes.fasta -outfmt 6 -evalue 0.01");
    ui->clearQueriesInfoText->setInfoText("Click this button to remove all queries in the below list.");
}
