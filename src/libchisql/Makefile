# src/libchisql: the chisql AST + SQL parser (sql.l/sql.y -> flex/bison
# -> sql-lexer.[ch]/sql-parser.[ch], generated - not checked into git).
# TOP is passed in by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -I$(TOP)/include -I$(TOP)/src/simclist -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP

SRCS = \
  create.c \
  ra.c \
  literal.c \
  common.c \
  insert.c \
  condition.c \
  expression.c \
  column.c \
  delete.c \
  sra.c \
  sql-parser.c \
  sql-lexer.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: libchisql.a

libchisql.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

# claude: sql.y and sql.l each #include the other's generated header
# (token defines / yylex prototype), so both must be generated before
# compiling either .o - but generating either one doesn't require the
# other, so the two generation rules are independent.
sql-parser.c sql-parser.h: sql.y
	$(YACC) -d -o sql-parser.c sql.y

sql-lexer.c sql-lexer.h: sql.l
	$(LEX) -o sql-lexer.c --header-file=sql-lexer.h sql.l

sql-parser.o sql-lexer.o: sql-lexer.h sql-parser.h

clean:
	rm -f *.o *.d libchisql.a sql-lexer.c sql-lexer.h sql-parser.c sql-parser.h

-include $(OBJS:.o=.d)
