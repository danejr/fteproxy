// This file is part of FTE.
//
// FTE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// FTE is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with FTE.  If not, see <http://www.gnu.org/licenses/>.

#include <cDFA.h>

#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/python/module.hpp>
#include <boost/python/def.hpp>
#include <boost/python/class.hpp>
#include <boost/algorithm/string.hpp>

#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"

typedef vector< std::string > split_vector_type;

DFA::DFA(std::string dfa_str, uint32_t max_len)
    : _max_len(max_len),
      _start_state(-1),
      _num_states(0),
      _num_symbols(0)
{
	// construct the _start_state, _final_states and symbols/states of our DFA
    std::vector<uint32_t> symbols;
    boost::unordered_set<uint32_t> states;
    std::string line;
    std::istringstream myfile(dfa_str);
    while ( getline (myfile,line) )
    {
        if (line.empty()) break;

        split_vector_type SplitVec;
        boost::split( SplitVec, line, boost::is_any_of("\t") );

        if (SplitVec.size() == 4) {
            uint32_t current_state = strtol(SplitVec[0].c_str(),NULL,10);
            uint32_t symbol = strtol(SplitVec[2].c_str(),NULL,10);
			states.insert( current_state );

            if (find(symbols.begin(), symbols.end(), symbol)==symbols.end()) {
                symbols.push_back( symbol );
            }

            if ( _start_state == -1 ) {
                _start_state = current_state;
            }
        } else if (SplitVec.size()==1) {
            uint32_t final_state = strtol(SplitVec[0].c_str(),NULL,10);
            _final_states.insert( final_state );
        } else {
        	// TODO: throw exception because we don't understand the file format
        }
        
    }
    states.insert( states.size() );
    
    _num_symbols = symbols.size();
    _num_states = states.size();

    // build up our sigma/sigma_reverse tables which enable mappings between
    // bytes/integers
    uint32_t j, k;
    for (j=0; j<_num_symbols; j++) {
    	_sigma[j] = (char)(symbols[j]-1);
    	_sigma_reverse[(char)(symbols[j]-1)] = j;
    }

    // intialize all transitions in our DFA to our dead state
    _delta.resize(boost::extents[_num_states][_num_symbols]);
    for (j=0; j<_num_states; j++) {
        for (k=0; k < _num_symbols; k++) {
        	_delta[j][k] = _num_states - 1;
        }
    }

    // fill our our transition function delta
    std::istringstream myfile2(dfa_str);
    while ( getline (myfile2,line) )
    {
        split_vector_type SplitVec;
        boost::split( SplitVec, line, boost::is_any_of("\t") );

        if (SplitVec.size() == 4) {
            uint32_t current_state = strtol(SplitVec[0].c_str(),NULL,10);
            uint32_t symbol = strtol(SplitVec[2].c_str(),NULL,10);
            uint32_t new_state = strtol(SplitVec[1].c_str(),NULL,10);
            
            symbol = _sigma_reverse[symbol];
            
            _delta[current_state][symbol] = new_state;
        }
    }

    // perform our precalculation to speed up (un)ranking
    DFA::_buildTable();
}


void DFA::_buildTable() {
    // TODO: baild if _final_states, _delta, or _T are not initialized

    uint32_t i;
    uint32_t q;
    uint32_t a;
    
    // ensure our table _T is the correct size
    _T.resize(boost::extents[_num_states][_max_len+1]);

    // set all _T[q][0] = 1 for all states in _final_states
    boost::unordered_set<uint32_t>::iterator state;
    for (state=_final_states.begin(); state!=_final_states.end(); state++) {
        _T[*state][0] = 1;
    }

    // walk through our table _T
    // we want each entry _T[q][i] to contain the number of strings that start
    // from state q, terminate in a final state, and are of length i
    for (i=1; i<=_max_len; i++) {
        for (q=0; q<_delta.size(); q++) {
            for (a=0; a<_delta[0].size(); a++) {
                uint32_t state = _delta[q][a];
                _T[q][i] += _T[state][i-1];
            }
        }
    }
}


std::string DFA::unrank(PyObject * c_in) {
    // TODO: throw exception if input integer is not in range of pre-computed
    //       values
    // TODO: throw exception if walking DFA does not end in a final state
    // TODO: throw exception if input integer is not PympzObject*-type

    std::string retval;

    // assume input integer c_in is PympzObject*
    // convert c_in to mpz_class
    mpz_class c = mpz_class(Pympz_AS_MPZ(c_in));

    // subtract values values from c, while increasing n, to determine
    // the length n of the string we're ranking
    uint32_t n = 1;
    while (c >= _T[_start_state][n]) {
        c -= _T[_start_state][n];
        n++;
    }

    // walk the DFA subtracting values from c until we have our n symbols
    uint32_t i, q = _start_state;
    uint32_t chars_left, char_cursor, state_cursor;
    for (i=1; i<=n; i++) {
        chars_left = n-i;
        char_cursor = 0;
        state_cursor = _delta[q][char_cursor];
        while (c >= _T[state_cursor][chars_left]) {
            c -= _T[state_cursor][chars_left];
            state_cursor =_delta[q][++char_cursor];
        }
        retval += _sigma[char_cursor];
        q = state_cursor;
    }

    return retval;
}

PyObject* DFA::rank( std::string X_in ) {
    // TODO: verify that input symbols are in alphabet of DFA

    PyObject* retval;

    // covert the input symbols in X_in into their numeric representation
    uint32_t i;
    array_type_uint32_t1 X(boost::extents[X_in.size()]);
    for (i=0; i<X_in.size(); i++) {
        X[i] = _sigma_reverse[X_in.at(i)];
    }

    // walk the DFA, adding values from T to c
    uint32_t n = X.size();
    uint32_t q = _start_state;
    uint32_t j;
    mpz_class c = 0;
    uint32_t state;
    for (i=1; i<=n; i++) {
        for (j=1; j<=X[i-1]; j++) {
            state = _delta[q][j-1];
            c += _T[state][n-i];
        }
        q = _delta[q][X[i-1]];
    }

    // bail if our final state is in _final_states
    if (_final_states.count(q)==0) {
        // TODO: throw exception, because we are not in a final state
    }

    // based on the length of our input string X, add values from
    // buildTable to put c in the correct slice
    for (i=0; i<n; i++) {
        c += _T[_start_state][i];
    }

    // convert our value c (mpz_class) to PyLong
    // TODO: figure out safer/faster way to convert mpz_t to PyLong (or gmpy.mpz)
    uint8_t base = 10;
    uint32_t retval_str_len = c.get_str().length();
    char *retval_str = new char[retval_str_len + 1];
    strcpy(retval_str, c.get_str().c_str());
    retval = PyLong_FromString(retval_str, NULL, base);

    return retval;
}

PyObject* DFA::getNumWordsInLanguage( uint32_t min_word_length,
                                      uint32_t max_word_length )
{
    // TODO: Figure out if there is a better way to convert an mpz_class to a PyLong

    PyObject* retval;

    // count the number of words in the language of length
    // at least min_word_length and no greater than max_word_length
    mpz_class num_words = 0;
    for (uint32_t word_length = min_word_length;
            word_length <= max_word_length;
            word_length++) {
        num_words += _T[_start_state][word_length];
    }

    // convert the resulting integer to a string
    uint8_t base = 10;
    uint32_t num_words_str_len = num_words.get_str().length();
    char *num_words_str = new char[num_words_str_len + 1];
    strcpy(num_words_str, num_words.get_str().c_str());
    retval = PyLong_FromString(num_words_str, NULL, base);

    // cleanup
    delete [] num_words_str;

    return retval;
}

std::string attFstFromRegex(std::string str_dfa)
{
    // TODO: Throw exception if DFA is not generated correctly (how do we do this?)
    // TODO: Identify when DFA has >N states, then throw exception

    std::string retval;

    // specify compile flags for re2
    re2::Regexp::ParseFlags re_flags;
    re_flags = re2::Regexp::ClassNL;
    re_flags = re_flags | re2::Regexp::OneLine;
    re_flags = re_flags | re2::Regexp::PerlClasses;
    re_flags = re_flags | re2::Regexp::PerlB;
    re_flags = re_flags | re2::Regexp::PerlX;
    re_flags = re_flags | re2::Regexp::Latin1;

    // compile regex to DFA
    RE2::Options opt;
    re2::Regexp* re = re2::Regexp::Parse( str_dfa, re_flags, NULL );
    re2::Prog* prog = re->CompileToProg( opt.max_mem() );
    retval = prog->PrintEntireDFA( re2::Prog::kFullMatch );

    // cleanup
    delete prog;
    re->Decref();

    return retval;
}

std::string attFstMinimize(std::string str_dfa)
{
    // TODO: Throw exception if fstcompile, fstminimize or fstprint don't exist in system path
    // TODO: Throw exception if we fail to generate abspath_dfa_min

    std::string retval;

    // create the destinations for our working files
    boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path();
    boost::filesystem::path temp_file = boost::filesystem::unique_path();

    std::string abspath_dfa     = temp_dir.native() + "/" + temp_file.native() + ".dfa";
    std::string abspath_fst     = temp_dir.native() + "/" + temp_file.native() + ".fst";
    std::string abspath_fst_min = temp_dir.native() + "/" + temp_file.native() + ".min.fst";
    std::string abspath_dfa_min = temp_dir.native() + "/" + temp_file.native() + ".min.dfa";

    // write our input DFA to disk
    std::ofstream dfa_stream;
    dfa_stream.open (abspath_dfa.c_str());
    dfa_stream << str_dfa;
    dfa_stream.close();

    std::string cmd;
    // convert our ATT DFA string to an FST
    cmd = "fstcompile " + abspath_dfa + " " + abspath_fst;
    system(cmd.c_str());

    // convert our FST to a minmized FST
    cmd = "fstminimize " + abspath_fst + " " + abspath_fst_min;
    system(cmd.c_str());

    // covert our minimized FST to an ATT FST string
    cmd = "fstprint " + abspath_fst_min + " " + abspath_dfa_min;
    system(cmd.c_str());

    // read the contents of of the file at abspath_dfa_min to our retval
    std::ifstream dfa_min_stream(abspath_dfa_min.c_str());
    std::stringstream buffer;
    buffer << dfa_min_stream.rdbuf();
    retval = std::string(buffer.str());
    dfa_min_stream.close();

    // cleanup
    remove( abspath_dfa.c_str() );
    remove( abspath_fst.c_str() );
    remove( abspath_fst_min.c_str() );
    remove( abspath_dfa_min.c_str() );

    return retval;
}

BOOST_PYTHON_MODULE(cDFA)
{
    boost::python::class_<DFA>("DFA",boost::python::init<std::string,int32_t>())
    .def("rank", &DFA::rank)
    .def("unrank", &DFA::unrank)
    .def("getNumWordsInLanguage", &DFA::getNumWordsInLanguage);

    boost::python::def("attFstFromRegex",attFstFromRegex);
    boost::python::def("attFstMinimize",attFstMinimize);
}