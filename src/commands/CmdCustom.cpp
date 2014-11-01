////////////////////////////////////////////////////////////////////////////////
//
// Copyright 2006 - 2014, Paul Beckingham, Federico Hernandez.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// http://www.opensource.org/licenses/mit-license.php
//
////////////////////////////////////////////////////////////////////////////////

#include <cmake.h>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <stdlib.h>
#include <Context.h>
#include <Filter.h>
#include <Lexer.h>
#include <ViewTask.h>
#include <i18n.h>
#include <text.h>
#include <main.h>
#include <CmdCustom.h>

extern Context context;

////////////////////////////////////////////////////////////////////////////////
CmdCustom::CmdCustom (
  const std::string& k,
  const std::string& u,
  const std::string& d)
{
  _keyword     = k;
  _usage       = u;
  _description = d;
  _read_only   = true;
  _displays_id = true;
}

////////////////////////////////////////////////////////////////////////////////
int CmdCustom::execute (std::string& output)
{
  int rc = 0;

  // Load report configuration.
  std::string reportColumns = context.config.get ("report." + _keyword + ".columns");
  std::string reportLabels  = context.config.get ("report." + _keyword + ".labels");
  std::string reportSort    = context.config.get ("report." + _keyword + ".sort");
  std::string reportFilter  = context.config.get ("report." + _keyword + ".filter");
  if (reportFilter != "")
    reportFilter = "( " + reportFilter + " )";

  std::vector <std::string> columns;
  split (columns, reportColumns, ',');
  validateReportColumns (columns);

  std::vector <std::string> labels;
  split (labels, reportLabels, ',');

  if (columns.size () != labels.size () && labels.size () != 0)
    throw format (STRING_CMD_CUSTOM_MISMATCH, _keyword);

  std::vector <std::string> sortOrder;
  split (sortOrder, reportSort, ',');
  validateSortColumns (sortOrder);

  // Prepend the argument list with those from the report filter.
  std::string lexeme;
  Lexer::Type type;
  Lexer lex (reportFilter);
  lex.ambiguity (false);
  while (lex.token (lexeme, type))
    context.cli.add (lexeme);

  // Reparse after tree change.
  context.cli.analyze ();

  // Apply filter.
  handleRecurrence ();
  Filter filter;
  std::vector <Task> filtered;
  filter.subset (filtered);

  // Sort the tasks.
  std::vector <int> sequence;
  for (unsigned int i = 0; i < filtered.size (); ++i)
    sequence.push_back (i);

  sort_tasks (filtered, sequence, reportSort);

  // Configure the view.
  ViewTask view;
  view.width (context.getWidth ());
  view.leftMargin (context.config.getInteger ("indent.report"));
  view.extraPadding (context.config.getInteger ("row.padding"));
  view.intraPadding (context.config.getInteger ("column.padding"));

  Color label (context.config.get ("color.label"));
  view.colorHeader (label);

  Color label_sort (context.config.get ("color.label.sort"));
  view.colorSortHeader (label_sort);

  Color alternate (context.config.get ("color.alternate"));
  view.colorOdd (alternate);
  view.intraColorOdd (alternate);

  // Capture columns that are sorted.
  std::vector <std::string> sortColumns;

  // Add the break columns, if any.
  std::vector <std::string>::iterator so;
  for (so = sortOrder.begin (); so != sortOrder.end (); ++so)
  {
    std::string name;
    bool ascending;
    bool breakIndicator;
    context.decomposeSortField (*so, name, ascending, breakIndicator);

    if (breakIndicator)
      view.addBreak (name);

    sortColumns.push_back (name);
  }

  // Add the columns and labels.
  for (unsigned int i = 0; i < columns.size (); ++i)
  {
    Column* c = Column::factory (columns[i], _keyword);
    if (i < labels.size ())
      c->setLabel (labels[i]);

    bool sort = std::find (sortColumns.begin (), sortColumns.end (), c->name ()) != sortColumns.end ()
                  ? true
                  : false;

    view.add (c, sort);
  }

  // How many lines taken up by table header?
  int table_header = 0;
  if (context.verbose ("label"))
  {
    if (context.color () && context.config.getBoolean ("fontunderline"))
      table_header = 1;  // Underlining doesn't use extra line.
    else
      table_header = 2;  // Dashes use an extra line.
  }

  // Report output can be limited by rows or lines.
  int maxrows = 0;
  int maxlines = 0;
  context.getLimits (maxrows, maxlines);

  // Adjust for fluff in the output.
  if (maxlines)
    maxlines -= table_header
              + (context.verbose ("blank") ? 1 : 0)
              + (context.verbose ("footnote") ? context.footnotes.size () : 0)
              + (context.verbose ("affected") ? 1 : 0)
              + context.config.getInteger ("reserved.lines");  // For prompt, etc.

  // Render.
  std::stringstream out;
  if (filtered.size ())
  {
    view.truncateRows (maxrows);
    view.truncateLines (maxlines);

    out << optionalBlankLine ()
        << view.render (filtered, sequence)
        << optionalBlankLine ();

    // Print the number of rendered tasks
    if (context.verbose ("affected"))
    {
      out << (filtered.size () == 1
                ? STRING_CMD_CUSTOM_COUNT
                : format (STRING_CMD_CUSTOM_COUNTN, filtered.size ()));

      if (maxrows && maxrows < (int)filtered.size ())
        out << ", " << format (STRING_CMD_CUSTOM_SHOWN, maxrows);

      if (maxlines && maxlines < (int)filtered.size ())
        out << ", "
            << format (STRING_CMD_CUSTOM_TRUNCATED, maxlines - table_header);

      out << "\n";
    }
  }
  else
  {
    context.footnote (STRING_FEEDBACK_NO_MATCH);
    rc = 1;
  }

  feedback_backlog ();
  output = out.str ();
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
void CmdCustom::validateReportColumns (std::vector <std::string>& columns)
{
  std::vector <std::string>::iterator i;
  for (i = columns.begin (); i != columns.end (); ++i)
    legacyColumnMap (*i);
}

////////////////////////////////////////////////////////////////////////////////
void CmdCustom::validateSortColumns (std::vector <std::string>& columns)
{
  std::vector <std::string>::iterator i;
  for (i = columns.begin (); i != columns.end (); ++i)
    legacySortColumnMap (*i);
}

////////////////////////////////////////////////////////////////////////////////
