//----------------------------------------------------------------------------
/** @file Book.cpp
*/
//----------------------------------------------------------------------------

#include <cmath>
#include <boost/numeric/conversion/bounds.hpp>

#include "BitsetIterator.hpp"
#include "Book.hpp"
#include "Time.hpp"

using namespace benzene;

//----------------------------------------------------------------------------

/** Dump debug info. */
#define OUTPUT_OB_INFO 1

//----------------------------------------------------------------------------

/** Current version for book databases. 
    Update this if BookNode changes to prevent old out-of-date books
    being loaded. */
const std::string Book::BOOK_DB_VERSION = "BENZENE_BOOK_VER_0001";

//----------------------------------------------------------------------------

bool BookNode::IsTerminal() const
{
    if (HexEvalUtil::IsWinOrLoss(m_value))
        return true;
    return false;
}

bool BookNode::IsLeaf() const
{
    return m_count == 0;
}

std::string BookNode::toString() const
{
    std::ostringstream os;
    os << std::showpos << std::fixed << std::setprecision(3);
    os << "Prop=" << m_value;
    os << std::noshowpos << ", ExpP=" << m_priority;
    os << std::showpos << ", Heur=" << m_heurValue << ", Cnt=" << m_count;
    return os.str();
}

//----------------------------------------------------------------------------

float BookUtil::Value(const BookNode& node, const HexState& state)
{
    if (state.Position().IsLegal(SWAP_PIECES))
        return std::max(node.m_value, BookUtil::InverseEval(node.m_value));
    return node.m_value;
}

float BookUtil::Score(const BookNode& node, const HexState& state, 
                      float countWeight)
{
    float score = BookUtil::InverseEval(Value(node, state));
    if (!node.IsTerminal())
        score += log(node.m_count + 1) * countWeight;
    return score;	
}

float BookUtil::InverseEval(float eval)
{
    if (HexEvalUtil::IsWinOrLoss(eval))
        return -eval;
    if (eval < 0 || eval > 1)
        LogInfo() << "eval = " << eval << '\n';
    HexAssert(0 <= eval && eval <= 1.0);
    return 1.0 - eval;
}

//----------------------------------------------------------------------------

int BookUtil::GetMainLineDepth(const Book& book, const HexState& origState)
{
    int depth = 0;
    HexState state(origState);
    for (;;) 
    {
        BookNode node;
        if (!book.Get(state, node))
            break;
        HexPoint move = INVALID_POINT;
        float value = -1e9;
        for (BitsetIterator p(state.Position().GetEmpty()); p; ++p)
        {
            state.PlayMove(*p);
            BookNode child;
            if (book.Get(state, child))
            {
                float curValue = InverseEval(BookUtil::Value(child, state));
                if (curValue > value)
                {
                    value = curValue;
                    move = *p;
                }
            }
            state.UndoMove(*p);
        }
        if (move == INVALID_POINT)
            break;
        state.PlayMove(move);
        depth++;
    }
    return depth;
}

namespace 
{

std::size_t TreeSize(const Book& book, HexState& state, 
                     StateMap<std::size_t>& solved)
{
    if (solved.Exists(state))
        return solved[state];
    BookNode node;
    if (!book.Get(state, node))
        return 0;
    std::size_t ret = 1;
    for (BitsetIterator p(state.Position().GetEmpty()); p; ++p) 
    {
        state.PlayMove(*p);
        ret += TreeSize(book, state, solved);
        state.UndoMove(*p);
    }
    solved[state] = ret;
    return ret;
}

}

std::size_t BookUtil::GetTreeSize(const Book& book, const HexState& origState)
{
    StateMap<std::size_t> solved;
    HexState state(origState);
    return TreeSize(book, state, solved);
}

//----------------------------------------------------------------------------

unsigned BookUtil::NumChildren(const Book& book, const HexState& origState)
{
    unsigned num = 0;
    HexState state(origState);
    for (BitsetIterator i(state.Position().GetEmpty()); i; ++i) 
    {
	state.PlayMove(*i);
	BookNode child;
        if (book.Get(state, child))
            ++num;
        state.UndoMove(*i);
    }
    return num;
}

void BookUtil::UpdateValue(const Book& book, BookNode& node, HexState& state)
{
    bool hasChild = false;
    float bestValue = boost::numeric::bounds<float>::lowest();
    for (BitsetIterator i(state.Position().GetEmpty()); i; ++i) 
    {
	state.PlayMove(*i);
	BookNode child;
        if (book.Get(state, child))
        {
            hasChild = true;
            float value = BookUtil::InverseEval(BookUtil::Value(child, state));
            if (value > bestValue)
		bestValue = value;
	    
        }
        state.UndoMove(*i);
    }
    if (hasChild)
        node.m_value = bestValue;
}

/** @todo Maybe switch this to take a bestChildValue instead of of a
    parent node. This would require flipping the parent in the caller
    function and reverse the order of the subtraction. */
float BookUtil::ComputePriority(const HexState& state, 
                                const BookNode& parent,
                                const BookNode& child,
                                double alpha)
{
    // Must adjust child value for swap, but not the parent because we
    // are comparing with the best child's value, ie, the minmax
    // value.
    float delta = parent.m_value 
        - BookUtil::InverseEval(BookUtil::Value(child, state));
    HexAssert(delta >= 0.0);
    HexAssert(child.m_priority >= BookNode::LEAF_PRIORITY);
    HexAssert(child.m_priority < BookNode::DUMMY_PRIORITY);
    return alpha * delta + child.m_priority + 1;
}

HexPoint BookUtil::UpdatePriority(const Book& book, BookNode& node, 
                                  HexState& state, float alpha)
{
    bool hasChild = false;
    float bestPriority = boost::numeric::bounds<float>::highest();
    HexPoint bestChild = INVALID_POINT;
    for (BitsetIterator i(state.Position().GetEmpty()); i; ++i) 
    {
	state.PlayMove(*i);
	BookNode child;
        if (book.Get(state, child))
        {
            hasChild = true;
            float priority 
                = BookUtil::ComputePriority(state, node, child, alpha);
            if (priority < bestPriority)
            {
                bestPriority = priority;
                bestChild = *i;
            }
        }
        state.UndoMove(*i);
    }
    if (hasChild)
        node.m_priority = bestPriority;
    return bestChild;
}

//----------------------------------------------------------------------------

HexPoint BookUtil::BestMove(const Book& book, const HexState& origState,
                            unsigned minCount, float countWeight)
{
    BookNode node;
    if (!book.Get(origState, node) || node.m_count < minCount)
        return INVALID_POINT;

    float bestScore = -1e9;
    HexPoint bestChild = INVALID_POINT;
    HexState state(origState);
    for (BitsetIterator p(state.Position().GetEmpty()); p; ++p)
    {
        state.PlayMove(*p);
        BookNode child;
        if (book.Get(state, child))
        {
            float score = BookUtil::Score(child, state, countWeight);
            if (score > bestScore)
            {
                bestScore = score;
                bestChild = *p;
            }
        }
        state.UndoMove(*p);
    }
    HexAssert(bestChild != INVALID_POINT);
    return bestChild;
}

//----------------------------------------------------------------------------

void BookUtil::DumpVisualizationData(const Book& book, HexState& state, 
                                     int depth, std::ostream& out)
{
    BookNode node;
    if (!book.Get(state, node))
        return;
    if (node.IsLeaf())
    {
        out << BookUtil::Value(node, state) << " " << depth << '\n';
        return;
    }
    for (BitsetIterator i(state.Position().GetEmpty()); i; ++i) 
    {
	state.PlayMove( *i);
        DumpVisualizationData(book, state, depth + 1, out);
        state.UndoMove(*i);
    }
}

namespace {

void DumpPolarizedLeafs(const Book& book, HexState& state,
                        float polarization, StateSet& seen,
                        PointSequence& pv, std::ostream& out,
                        const StateSet& ignoreSet)
{
    if (seen.Exists(state))
        return;
    BookNode node;
    if (!book.Get(state, node))
        return;
    if (fabs(BookUtil::Value(node, state) - 0.5) >= polarization 
        && node.IsLeaf() && !node.IsTerminal()
        && ignoreSet.Exists(state))
    {
        out << HexPointUtil::ToString(pv) << '\n';
        seen.Insert(state);
    }
    else
    {
        if (node.IsLeaf() || node.IsTerminal())
            return;
        for (BitsetIterator i(state.Position().GetEmpty()); i; ++i) 
        {
            state.PlayMove(*i);
            pv.push_back(*i);
            DumpPolarizedLeafs(book, state, polarization, seen, pv, out, 
                               ignoreSet);
            pv.pop_back();
            state.UndoMove(*i);
        }
        seen.Insert(state);
    } 
}

}

void BookUtil::DumpPolarizedLeafs(const Book& book, HexState& state,
                                  float polarization, PointSequence& pv, 
                                  std::ostream& out, 
                                  const StateSet& ignoreSet)
{
    StateSet seen;
    ::DumpPolarizedLeafs(book, state, polarization, seen, pv, out, ignoreSet);
}

void BookUtil::ImportSolvedStates(Book& book, const ConstBoard& constBoard,
                                  std::istream& positions)
{
    StoneBoard brd(constBoard.Width(), constBoard.Height());
    HexState state(brd, FIRST_TO_PLAY);
    std::string text;
    std::size_t lineNumber = 0;
    std::size_t numParsed = 0;
    std::size_t numReplaced = 0;
    std::size_t numNew = 0;
    while (std::getline(positions, text))
    {
        ++lineNumber;
        std::istringstream is(text);
        PointSequence points;
        HexColor winner = EMPTY;
        bool parsed = false;
        while (true)
        {
            std::string token;
            is >> token;
            if (token == "black")
            {
                winner = BLACK;
                parsed = true;
                break;
            } 
            else if (token == "white")
            {
                winner = WHITE;
                parsed = true;
                break;
            }
            else
            {
                HexPoint p = HexPointUtil::FromString(token);
                if (p == INVALID_POINT)
                    break;
                points.push_back(p);
            }
        }
        if (!parsed)
        {
            LogInfo() << "Skipping badly formed line " << lineNumber << ".\n";
            continue;
        }
        HexAssert(winner != EMPTY);
        
        ++numParsed;
        state.Position().StartNewGame();
        state.SetToPlay(FIRST_TO_PLAY);
        for (std::size_t i = 0; i < points.size(); ++i)
            state.PlayMove(points[i]);
        HexEval ourValue = (state.ToPlay() == winner) 
            ? IMMEDIATE_WIN : IMMEDIATE_LOSS;
        BookNode node;
        if (book.Get(state, node))
        {
            HexAssert(node.IsLeaf());
            HexAssert(!node.IsTerminal());
            node.m_value = ourValue;
            ++numReplaced;
        }
        else 
        {
            node = BookNode(ourValue);
            ++numNew;
        }
        book.Put(state, node);
    }
    book.Flush();
    LogInfo() << "   Lines: " << lineNumber << '\n';
    LogInfo() << "  Parsed: " << numParsed << '\n';
    LogInfo() << "Replaced: " << numReplaced << '\n';
    LogInfo() << "     New: " << numNew << '\n';
}

//----------------------------------------------------------------------------
