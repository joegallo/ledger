#if defined(__GNUG__) && __GNUG__ < 3
#define _XOPEN_SOURCE
#endif

#include "journal.h"
#include "textual.h"
#include "datetime.h"
#include "valexpr.h"
#include "error.h"
#include "option.h"
#include "config.h"
#include "timing.h"
#include "util.h"
#include "acconf.h"
#ifdef USE_BOOST_PYTHON
#include "py_eval.h"
#endif

#include <fstream>
#include <sstream>
#include <cstring>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#if defined(__GNUG__) && __GNUG__ < 3
extern "C" char *realpath(const char *, char resolved_path[]);
#endif

#define TIMELOG_SUPPORT 1

namespace ledger {

#define MAX_LINE 1024

static std::string  path;
static unsigned int linenum;

#ifdef TIMELOG_SUPPORT
static std::time_t time_in;
static account_t * last_account;
static std::string last_desc;
#endif

inline char * next_element(char * buf, bool variable = false)
{
  for (char * p = buf; *p; p++) {
    if (! (*p == ' ' || *p == '\t'))
      continue;

    if (! variable) {
      *p = '\0';
      return skip_ws(p + 1);
    }
    else if (*p == '\t') {
      *p = '\0';
      return skip_ws(p + 1);
    }
    else if (*(p + 1) == ' ') {
      *p = '\0';
      return skip_ws(p + 2);
    }
  }
  return NULL;
}

void parse_amount(const char * text, amount_t& amt, unsigned short flags,
		  transaction_t& xact)
{
  if (*text != '(') {
    amt.parse(text, flags);
  } else {
    value_expr_t * expr = parse_value_expr(text);
    value_t result;
    expr->compute(result, details_t(xact));
    switch (result.type) {
    case value_t::BOOLEAN:
      amt = *((bool *) result.data);
      break;
    case value_t::INTEGER:
      amt = *((long *) result.data);
      break;
    case value_t::AMOUNT:
      amt = *((amount_t *) result.data);
      break;

    case value_t::BALANCE:
    case value_t::BALANCE_PAIR:
      throw parse_error(path, linenum, "Value expression yields a balance");
      break;
    }
  }
}

transaction_t * parse_transaction(char * line, account_t * account)
{
  // The account will be determined later...

  std::auto_ptr<transaction_t> xact(new transaction_t(NULL));

  // The call to `next_element' will skip past the account name,
  // and return a pointer to the beginning of the amount.  Once
  // we know where the amount is, we can strip off any
  // transaction note, and parse it.

  char * p = skip_ws(line);
  if (char * cost_str = next_element(p, true)) {
    cost_str = skip_ws(cost_str);
    bool has_amount = *cost_str;

    if (char * note_str = std::strchr(cost_str, ';')) {
      if (cost_str == note_str)
	has_amount = false;
      *note_str++ = '\0';
      xact->note = skip_ws(note_str);
    }

    if (has_amount) {
      bool   per_unit  = true;
      char * price_str = std::strchr(cost_str, '@');
      if (price_str) {
	if (price_str == cost_str)
	  throw parse_error(path, linenum, "Cost specified without amount");

	*price_str++ = '\0';
	if (*price_str == '@') {
	  per_unit = false;
	  price_str++;
	}
      }
      parse_amount(skip_ws(cost_str), xact->amount, AMOUNT_PARSE_NO_REDUCE,
		   *xact);
      if (price_str) {
	xact->cost = new amount_t;
	parse_amount(skip_ws(price_str), *xact->cost, AMOUNT_PARSE_NO_MIGRATE,
		     *xact);
      }

      if (price_str && per_unit) {
	*xact->cost *= xact->amount;
	*xact->cost = xact->cost->round(xact->cost->commodity().precision);
      }
      xact->amount.reduce();
    }
  }

  char * q = p + std::strlen(p) - 1;
  while (q >= p && std::isspace(*q))
    *q-- = '\0';

  if (*p == '[' || *p == '(') {
    xact->flags |= TRANSACTION_VIRTUAL;
    if (*p == '[')
      xact->flags |= TRANSACTION_BALANCE;
    p++;

    char * e = p + (std::strlen(p) - 1);
    assert(*e == ')' || *e == ']');
    *e = '\0';
  }

  xact->account = account->find_account(p);

  return xact.release();
}

bool parse_transactions(std::istream&	   in,
			account_t *	   account,
			entry_base_t&	   entry,
			const std::string& kind)
{
  static char line[MAX_LINE + 1];
  bool	      added = false;

  while (! in.eof() && (in.peek() == ' ' || in.peek() == '\t')) {
    in.getline(line, MAX_LINE);
    if (in.eof())
      break;
    linenum++;
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\r') {
      char * p = skip_ws(line);
      if (! *p || *p == '\r')
	break;
    }
    if (transaction_t * xact = parse_transaction(line, account)) {
      entry.add_transaction(xact);
      added = true;
    }
  }

  return added;
}

namespace {
  TIMER_DEF(entry_finish,  "finalizing entry");
  TIMER_DEF(entry_xacts,   "parsing transactions");
  TIMER_DEF(entry_details, "parsing entry details");
  TIMER_DEF(entry_date,    "parsing entry date");
}

entry_t * parse_entry(std::istream& in, char * line, account_t * master,
		      textual_parser_t& parser)
{
  std::auto_ptr<entry_t> curr(new entry_t);

  // Parse the date

  TIMER_START(entry_date);

  char * next = next_element(line);

  if (! quick_parse_date(line, &curr->date))
    throw parse_error(path, linenum, "Failed to parse date");

  TIMER_STOP(entry_date);

  // Parse the optional cleared flag: *

  TIMER_START(entry_details);

  if (next && *next == '*') {
    curr->state = entry_t::CLEARED;
    next = skip_ws(++next);
  }

  // Parse the optional code: (TEXT)

  if (next && *next == '(') {
    if (char * p = std::strchr(next++, ')')) {
      *p++ = '\0';
      curr->code = next;
      next = skip_ws(p);
    }
  }

  // Parse the description text

  curr->payee = next ? next : "<Unspecified payee>";

  TIMER_STOP(entry_details);

  // Parse all of the transactions associated with this entry

  TIMER_START(entry_xacts);

  while (! in.eof() && (in.peek() == ' ' || in.peek() == '\t')) {
    in.getline(line, MAX_LINE);
    if (in.eof())
      break;
    linenum++;
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\r') {
      char * p = skip_ws(line);
      if (! *p || *p == '\r')
	break;
    }
    if (transaction_t * xact = parse_transaction(line, master))
      curr->add_transaction(xact);
  }

  TIMER_STOP(entry_xacts);

  return curr.release();
}

template <typename T>
struct push_var {
  T& var;
  T prev;
  push_var(T& _var) : var(_var), prev(var) {}
  ~push_var() { var = prev; }
};

static inline void parse_symbol(char *& p, std::string& symbol)
{
  if (*p == '"') {
    char * q = std::strchr(p + 1, '"');
    if (! q)
      throw parse_error(path, linenum,
			"Quoted commodity symbol lacks closing quote");
    symbol = std::string(p + 1, 0, q - p - 1);
    p = q + 2;
  } else {
    char * q = std::strchr(p, ' ');
    if (q) {
      *q = '\0';
      symbol = std::string(p, 0, q - p);
      p = q + 1;
    } else {
      symbol = p;
      p += symbol.length();
    }
  }
  if (symbol.empty())
    throw parse_error(path, linenum, "Failed to parse commodity");
}

unsigned int textual_parser_t::parse(std::istream&	 in,
				     journal_t *	 journal,
				     account_t *	 master,
				     const std::string * original_file)
{
  static bool   added_auto_entry_hook = false;
  static char   line[MAX_LINE + 1];
  char		c;
  unsigned int  count = 0;
  unsigned int  errors = 0;

  std::list<account_t *> account_stack;
  auto_entry_finalizer_t auto_entry_finalizer(journal);

  if (! master)
    master = journal->master;

  account_stack.push_front(master);

  path	  = journal->sources.back();
  linenum = 1;

  while (in.good() && ! in.eof()) {
    try {
      istream_pos_type beg_pos = in.tellg();

      in.getline(line, MAX_LINE);
      if (in.eof())
	break;
      linenum++;

      switch (line[0]) {
      case '\0':
      case '\r':
	break;

      case ' ':
      case '\t': {
	char * p = skip_ws(line);
	if (*p && *p != '\r')
	  throw parse_error(path, linenum - 1, "Line begins with whitespace");
	break;
      }

#ifdef TIMELOG_SUPPORT
      case 'i':
      case 'I': {
	std::string date(line, 2, 19);

	char * p = skip_ws(line + 22);
	char * n = next_element(p, true);
	last_desc = n ? n : "";

	struct std::tm when;
	if (strptime(date.c_str(), "%Y/%m/%d %H:%M:%S", &when)) {
	  time_in      = std::mktime(&when);
	  last_account = account_stack.front()->find_account(p);
	} else {
	  last_account = NULL;
	  throw parse_error(path, linenum, "Cannot parse timelog entry date");
	}
	break;
      }

      case 'o':
      case 'O':
	if (last_account) {
	  std::string date(line, 2, 19);

	  char * p = skip_ws(line + 22);
	  if (last_desc.empty() && *p)
	    last_desc = p;

	  struct std::tm when;
	  if (strptime(date.c_str(), "%Y/%m/%d %H:%M:%S", &when)) {
	    std::auto_ptr<entry_t> curr(new entry_t);
	    curr->date  = std::mktime(&when);
	    curr->state = entry_t::CLEARED;
	    curr->code  = "";
	    curr->payee = last_desc;

	    double diff = std::difftime(curr->date, time_in);
	    char   buf[32];
	    std::sprintf(buf, "%lds", long(diff));
	    amount_t amt;
	    amt.parse(buf);

	    transaction_t * xact
	      = new transaction_t(last_account, amt, TRANSACTION_VIRTUAL);
	    curr->add_transaction(xact);

	    if (! journal->add_entry(curr.get()))
	      throw parse_error(path, linenum,
				"Failed to record 'out' timelog entry");
	    else
	      curr.release();

	    count++;
	  } else {
	    throw parse_error(path, linenum, "Cannot parse timelog entry date");
	  }

	  last_account = NULL;
	}
	break;
#endif // TIMELOG_SUPPORT

      case 'D':			// a default commodity for "entry"
	commodity_t::default_commodity =
	  commodity_t::find_commodity(skip_ws(line + 1), true);
	break;

      case 'C':			// a set of conversions
	if (char * p = std::strchr(line + 1, '=')) {
	  *p++ = '\0';
	  parse_conversion(line + 1, p);
	}
	break;

      case 'P': {		// a pricing entry
	std::time_t date;

	char * b = skip_ws(line + 1);
	char * p = std::strchr(b, ' ');
	if (! p) break;
	*p++ = '\0';

	if (! quick_parse_date(b, &date))
	  throw parse_error(path, linenum, "Failed to parse date");

	int hour, min, sec;

	if (! *p) break; hour = (*p++ - '0') * 10;
	if (! *p) break; hour += *p++ - '0';
	p++; if (! *p) break;
	if (! *p) break; min = (*p++ - '0') * 10;
	if (! *p) break; min += *p++ - '0';
	p++; if (! *p) break;
	if (! *p) break; sec = (*p++ - '0') * 10;
	if (! *p) break; sec += *p++ - '0';
	p++; if (! *p) break;

	date = std::time_t(((unsigned long) date) +
			   hour * 3600 + min * 60 + sec);

	std::string symbol;
	amount_t    price;

	parse_symbol(p, symbol);
	price.parse(skip_ws(p));

	commodity_t * commodity = commodity_t::find_commodity(symbol, true);
	commodity->add_price(date, price);
	break;
      }

      case 'N': {			// don't download prices
	char * p = skip_ws(line + 1);
	std::string symbol;
	parse_symbol(p, symbol);

	commodity_t * commodity = commodity_t::find_commodity(symbol, true);
	commodity->flags |= COMMODITY_STYLE_NOMARKET;
	break;
      }

      case 'Y':                   // set the current year
	now_year = std::atoi(skip_ws(line + 1)) - 1900;
	break;

#ifdef TIMELOG_SUPPORT
      case 'h':
      case 'b':
#endif
      case ';':                   // a comment line
	break;

      case '-': {                 // option setting
	char * p = std::strchr(line, ' ');
	if (! p)
	  p = std::strchr(line, '=');
	if (p)
	  *p++ = '\0';
	process_option(config_options, line + 2, p ? skip_ws(p) : NULL);
	break;
      }

      case '=': {		// automated entry
	if (! added_auto_entry_hook) {
	  journal->add_entry_finalizer(&auto_entry_finalizer);
	  added_auto_entry_hook = true;
	}

	auto_entry_t * ae = new auto_entry_t(skip_ws(line + 1));
	if (parse_transactions(in, account_stack.front(), *ae, "automated")) {
	  if (ae->finalize()) {
	    journal->auto_entries.push_back(ae);
	    ae->src_idx = journal->sources.size() - 1;
	    ae->beg_pos = beg_pos;
	    ae->end_pos = in.tellg();
	  } else {
	    throw parse_error(path, linenum,
			      "Automated entry failed to balance");
	  }
	}
	break;
      }

      case '~': {		// period entry
	period_entry_t * pe = new period_entry_t(skip_ws(line + 1));
	if (! pe->period)
	  throw parse_error(path, linenum,
			    std::string("Parsing time period '") + line + "'");

	if (parse_transactions(in, account_stack.front(), *pe, "period")) {
	  if (pe->finalize()) {
	    extend_entry_base(journal, *pe);
	    journal->period_entries.push_back(pe);
	    pe->src_idx = journal->sources.size() - 1;
	    pe->beg_pos = beg_pos;
	    pe->end_pos = in.tellg();
	  } else {
	    throw parse_error(path, linenum, "Period entry failed to balance");
	  }
	}
	break;
      }

      case '!': {                 // directive
	char * p = std::strchr(line, ' ');
	if (p)
	  *p++ = '\0';
	std::string word(line + 1);
	if (word == "include") {
	  push_var<unsigned int> save_linenum(linenum);
	  push_var<std::string>  save_path(path);

	  path = skip_ws(p);
	  if (path[0] != '/' && path[0] != '\\') {
	    std::string::size_type pos = save_path.prev.rfind('/');
	    if (pos == std::string::npos)
	      pos = save_path.prev.rfind('\\');
	    if (pos != std::string::npos)
	      path = std::string(save_path.prev, 0, pos + 1) + path;
	  }

	  DEBUG_PRINT("ledger.textual.include",
		      "Including path '" << path << "'");
	  count += parse_journal_file(path, journal, account_stack.front());
	}
	else if (word == "account") {
	  account_t * acct;
	  acct = account_stack.front()->find_account(skip_ws(p));
	  account_stack.push_front(acct);
	}
	else if (word == "end") {
	  account_stack.pop_front();
	}
#ifdef USE_BOOST_PYTHON
	else if (word == "python") {
	  python_eval(in, PY_EVAL_MULTI);
	}
#endif
	break;
      }

      default: {
	unsigned int first_line = linenum;

	if (entry_t * entry = parse_entry(in, line, account_stack.front(),
					  *this)) {
	  if (journal->add_entry(entry)) {
	    entry->src_idx = journal->sources.size() - 1;
	    entry->beg_pos = beg_pos;
	    entry->end_pos = in.tellg();
	    count++;
	  } else {
	    print_entry(std::cerr, *entry);
	    delete entry;
	    throw parse_error(path, first_line, "Entry above does not balance");
	  }
	} else {
	  throw parse_error(path, first_line, "Failed to parse entry");
	}
	break;
      }
      }
    }
    catch (const parse_error& err) {
      std::cerr << "Error: " << err.what() << std::endl;
      errors++;
    }
    catch (const amount_error& err) {
      std::cerr << "Error: " << path << ", line " << (linenum - 1) << ": "
		<< err.what() << std::endl;;
      errors++;
    }
    catch (const error& err) {
      std::cerr << "Error: " << path << ", line " << (linenum - 1) << ": "
		<< err.what() << std::endl;;
      errors++;
    }
  }

 done:
  if (added_auto_entry_hook)
    journal->remove_entry_finalizer(&auto_entry_finalizer);

  if (errors > 0)
    throw error(std::string("Errors parsing file '") + path + "'");

  return count;
}

void write_textual_journal(journal_t& journal, std::string path,
			   item_handler<transaction_t>& formatter,
			   std::ostream& out)
{
  unsigned long index = 0;
  std::string   found;
  char          buf1[PATH_MAX];
  char          buf2[PATH_MAX];

#ifdef HAVE_REALPATH
  ::realpath(path.c_str(), buf1);
  for (strings_list::iterator i = journal.sources.begin();
       i != journal.sources.end();
       i++) {
    ::realpath((*i).c_str(), buf2);
    if (std::strcmp(buf1, buf2) == 0) {
      found = *i;
      break;
    }
    index++;
  }
#else
  for (strings_list::iterator i = journal.sources.begin();
       i != journal.sources.end();
       i++) {
    if (path == *i) {
      found = *i;
      break;
    }
    index++;
  }
#endif

  if (found.empty())
    throw error(std::string("Journal does not refer to file '") +
		found + "'");

  entries_list::iterator	el = journal.entries.begin();
  auto_entries_list::iterator	al = journal.auto_entries.begin();
  period_entries_list::iterator pl = journal.period_entries.begin();

  istream_pos_type pos = 0;
  istream_pos_type jump_to;

  format_t	hdr_fmt(config.write_hdr_format);
  std::ifstream in(found.c_str());

  while (! in.eof()) {
    entry_base_t * base = NULL;
    if (el != journal.entries.end() &&
	pos == (*el)->beg_pos) {
      hdr_fmt.format(out, details_t(**el));
      base = *el++;
    }
    else if (al != journal.auto_entries.end() &&
	     pos == (*al)->beg_pos) {
      out << "= " << (*al)->predicate_string << '\n';
      base = *al++;
    }
    else if (pl != journal.period_entries.end() &&
	     pos == (*pl)->beg_pos) {
      out << "~ " << (*pl)->period_string << '\n';
      base = *pl++;
    }

    char c;
    if (base) {
      for (transactions_list::iterator x = base->transactions.begin();
	   x != base->transactions.end();
	   x++)
	if (! ((*x)->flags & TRANSACTION_AUTO)) {
	  transaction_xdata(**x).dflags |= TRANSACTION_TO_DISPLAY;
	  formatter(**x);
	}
      formatter.flush();

      while (pos < base->end_pos) {
	in.get(c);
	pos = in.tellg(); // pos++;
      }
    } else {
      in.get(c);
      pos = in.tellg(); // pos++;
      out.put(c);
    }
  }
}

} // namespace ledger

#ifdef USE_BOOST_PYTHON

#include <boost/python.hpp>

using namespace boost::python;
using namespace ledger;

BOOST_PYTHON_MEMBER_FUNCTION_OVERLOADS(textual_parse_overloads,
				       textual_parser_t::parse, 2, 4)

void export_textual() {
  class_< textual_parser_t, bases<parser_t> > ("TextualParser")
    .def("test", &textual_parser_t::test)
    .def("parse", &textual_parser_t::parse, textual_parse_overloads())
    ;
}

#endif // USE_BOOST_PYTHON
