/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NodeGraphUndoRedo.h"

#include <algorithm> // min, max
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QtCore/QDebug>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/CreateNodeArgs.h"
#include "Engine/GroupInput.h"
#include "Engine/GroupOutput.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerNode.h"
#include "Engine/ViewerInstance.h"

#include "Gui/NodeGui.h"
#include "Gui/NodeGraph.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/Edge.h"
#include "Gui/GuiApplicationManager.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/NodeClipBoard.h"

#define MINIMUM_VERTICAL_SPACE_BETWEEN_NODES 10

NATRON_NAMESPACE_ENTER;

MoveMultipleNodesCommand::MoveMultipleNodesCommand(const NodesGuiList & nodes,
                                                   double dx,
                                                   double dy,
                                                   QUndoCommand *parent)
    : QUndoCommand(parent)
    , _firstRedoCalled(false)
    , _nodes()
    , _dx(dx)
    , _dy(dy)
{
    assert( !nodes.empty() );
    for (NodesGuiList::const_iterator it = nodes.begin(); it!=nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
}

void
MoveMultipleNodesCommand::move(double dx,
                               double dy)
{
    for (std::list<NodeGuiWPtr>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodeGuiPtr n = it->lock();
        if (!n) {
            continue;
        }
        QPointF pos = n->pos();
        n->setPosition(pos.x() + dx, pos.y() + dy);
    }
}

void
MoveMultipleNodesCommand::undo()
{
    move(-_dx, -_dy);
    setText( tr("Move nodes") );
}

void
MoveMultipleNodesCommand::redo()
{
    if (_firstRedoCalled) {
        move(_dx, _dy);
    }
    _firstRedoCalled = true;
    setText( tr("Move nodes") );
}

AddMultipleNodesCommand::AddMultipleNodesCommand(NodeGraph* graph,
                                                 const NodesList& nodes,
                                                 QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
    , _firstRedoCalled(false)
    , _isUndone(false)
{
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
    setText( tr("Add node") );

}

AddMultipleNodesCommand::AddMultipleNodesCommand(NodeGraph* graph,
                                                 const NodePtr & node,
                                                 QUndoCommand* parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
    , _firstRedoCalled(false)
    , _isUndone(false)
{
    _nodes.push_back(node);
    setText( tr("Add node") );

}

AddMultipleNodesCommand::~AddMultipleNodesCommand()
{
    // If the nodes we removed due to a undo, destroy them
    if (_isUndone) {
        for (NodesWList::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
            NodePtr node = it->lock();
            if (node) {
                node->destroyNode(false, false);
            }
        }
    }
}

void
AddMultipleNodesCommand::undo()
{
    _isUndone = true;
    std::list<ViewerInstancePtr> viewersToRefresh;


    for (NodesWList::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodePtr node = it->lock();
        if (!node) {
            continue;
        }

        node->deactivate(Node::eDeactivateFlagConnectOutputsToMainInput); // triggerRender
    }

    _graph->clearSelection();

    _graph->getGui()->getApp()->triggerAutoSave();
    _graph->getGui()->getApp()->renderAllViewers();


}

void
AddMultipleNodesCommand::redo()
{
    _isUndone = false;
    NodesList nodes;
    for (NodesWList::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodePtr n = it->lock();
        if (!n) {
            continue;
        }
        nodes.push_back(n);
    }
    if (nodes.empty()) {
        return;
    }
    if (_firstRedoCalled) {
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->activate(Node::eActivateFlagRestoreOutputs); //triggerRender
        }
    }


    if ( (nodes.size() != 1) || !nodes.front()->isEffectNodeGroup() ) {
        _graph->setSelection(nodes);
    }

    _graph->getGui()->getApp()->recheckInvalidLinks();
    _graph->getGui()->getApp()->triggerAutoSave();
    _graph->getGui()->getApp()->renderAllViewers();


    _firstRedoCalled = true;
}

RemoveMultipleNodesCommand::RemoveMultipleNodesCommand(NodeGraph* graph,
                                                       const std::list<NodeGuiPtr > & nodes,
                                                       QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
    , _isRedone(false)
{
    for (std::list<NodeGuiPtr >::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeToRemove n;
        n.node = *it;
        _nodes.push_back(n);
    }
}

RemoveMultipleNodesCommand::~RemoveMultipleNodesCommand()
{
    if (_isRedone) {
        for (std::list<NodeToRemove>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
            NodeGuiPtr n = it->node.lock();
            if (n) {
                n->getNode()->destroyNode(false, false);
            }
        }
    }
}

void
RemoveMultipleNodesCommand::undo()
{
    std::list<SequenceTime> allKeysToAdd;
    std::list<NodeToRemove>::iterator next = _nodes.begin();

    if ( next != _nodes.end() ) {
        ++next;
    }
    for (std::list<NodeToRemove>::iterator it = _nodes.begin();
         it != _nodes.end();
         ++it) {
        NodeGuiPtr node = it->node.lock();
        node->getNode()->activate(Node::eActivateFlagRestoreOutputs);


        // increment for next iteration
        if ( next != _nodes.end() ) {
            ++next;
        }
    } // for(it)


    _graph->getGui()->getApp()->triggerAutoSave();
    _graph->getGui()->getApp()->renderAllViewers();
    _graph->getGui()->getApp()->redrawAllViewers();
    _graph->updateNavigator();

    _isRedone = false;
    _graph->scene()->update();
    setText( tr("Remove node") );
} // RemoveMultipleNodesCommand::undo

void
RemoveMultipleNodesCommand::redo()
{
    _isRedone = true;

    std::list<NodeToRemove>::iterator next = _nodes.begin();
    if ( next != _nodes.end() ) {
        ++next;
    }
    for (std::list<NodeToRemove>::iterator it = _nodes.begin();
         it != _nodes.end();
         ++it) {
        NodeGuiPtr node = it->node.lock();
        ///Make a copy before calling deactivate which will modify the list
        OutputNodesMap outputs;
        node->getNode()->getOutputs(outputs);

        NodesList outputsToRestore;
        for (OutputNodesMap::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
            outputsToRestore.push_back(it2->first);
        }

        node->getNode()->deactivate(Node::eDeactivateFlagConnectOutputsToMainInput);


        if (_nodes.size() == 1) {
            ///If we're deleting a single node and there's a viewer in output,reconnect the viewer to another connected input it has
            for (OutputNodesMap::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                const NodePtr& output = it2->first;


                ///the output must be in the outputs to restore
                NodesList::const_iterator found = std::find(outputsToRestore.begin(), outputsToRestore.end(), output);

                if ( found != outputsToRestore.end() ) {
                    ViewerNodePtr isViewer = output->isEffectViewerNode();
                    ///if the node is an viewer, when disconnecting the active input just activate another input instead
                    if (isViewer) {
                        const std::vector<NodeWPtr> & inputs = output->getInputs();
                        ///set as active input the first non null input
                        for (std::size_t i = 0; i < inputs.size(); ++i) {
                            NodePtr input = inputs[i].lock();
                            if (input) {
                                isViewer->connectInputToIndex(i, 0);
                                break;
                            }
                        }
                    }
                }
            }
        }

        // increment for next iteration
        if ( next != _nodes.end() ) {
            ++next;
        }
    } // for(it)

    _graph->getGui()->getApp()->triggerAutoSave();
    _graph->getGui()->getApp()->renderAllViewers();
    _graph->getGui()->getApp()->redrawAllViewers();
    _graph->updateNavigator();

    _graph->scene()->update();
    setText( tr("Remove node") );
} // redo

ConnectCommand::ConnectCommand(NodeGraph* graph,
                               Edge* edge,
                               const NodeGuiPtr & oldSrc,
                               const NodeGuiPtr & newSrc,
                               int viewerInternalIndex,
                               QUndoCommand *parent)
    : QUndoCommand(parent),
    _oldSrc(oldSrc),
    _newSrc(newSrc),
    _dst( edge->getDest() ),
    _graph(graph),
    _inputNb( edge->getInputNumber() ),
    _viewerInternalIndex(viewerInternalIndex)
{
    assert( _dst.lock() );
}

void
ConnectCommand::undo()
{
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    doConnect(newSrc,
              oldSrc,
              dst,
              _inputNb,
              _viewerInternalIndex);

    if (newSrc) {
        setText( tr("Connect %1 to %2")
                 .arg( QString::fromUtf8( dst->getNode()->getLabel().c_str() ) ).arg( QString::fromUtf8( newSrc->getNode()->getLabel().c_str() ) ) );
    } else {
        setText( tr("Disconnect %1")
                 .arg( QString::fromUtf8( dst->getNode()->getLabel().c_str() ) ) );
    }


    ViewerInstancePtr isDstAViewer = dst->getNode()->isEffectViewerInstance();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }
    _graph->update();
} // undo

void
ConnectCommand::redo()
{
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    doConnect(oldSrc,
              newSrc,
              dst,
              _inputNb,
              _viewerInternalIndex);

    if (newSrc) {
        setText( tr("Connect %1 to %2")
                 .arg( QString::fromUtf8( dst->getNode()->getLabel().c_str() ) ).arg( QString::fromUtf8( newSrc->getNode()->getLabel().c_str() ) ) );
    } else {
        setText( tr("Disconnect %1")
                 .arg( QString::fromUtf8( dst->getNode()->getLabel().c_str() ) ) );
    }


    ViewerNodePtr isDstAViewer = dst->getNode()->isEffectViewerNode();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }
    _graph->update();
} // redo

void
ConnectCommand::doConnect(const NodeGuiPtr &oldSrc,
                          const NodeGuiPtr &newSrc,
                          const NodeGuiPtr& dst,
                          int inputNb,
                          int viewerInternalIndex)
{
    NodePtr internalDst =  dst->getNode();
    NodePtr internalNewSrc = newSrc ? newSrc->getNode() : NodePtr();
    NodePtr internalOldSrc = oldSrc ? oldSrc->getNode() : NodePtr();
    ViewerNodePtr isViewer = internalDst->isEffectViewerNode();


    if (isViewer) {
        ///if the node is an inspector  disconnect any current connection between the inspector and the _newSrc
        for (int i = 0; i < internalDst->getMaxInputCount(); ++i) {
            if ( (i != inputNb) && (internalDst->getInput(i) == internalNewSrc) ) {
                internalDst->disconnectInput(i);
            }
        }
    }
    if (internalOldSrc && internalNewSrc) {
        internalDst->swapInput(internalNewSrc, inputNb);
    } else {
        if (internalOldSrc && internalNewSrc) {
            Node::CanConnectInputReturnValue ret = internalDst->canConnectInput(internalNewSrc, inputNb);
            bool connectionOk = ret == Node::eCanConnectInput_ok ||
                                ret == Node::eCanConnectInput_differentFPS ||
                                ret == Node::eCanConnectInput_differentPars;
            if (connectionOk) {
                internalDst->swapInput(internalNewSrc, inputNb);
            } else {
                std::list<int> inputsConnectedToOldSrc = internalOldSrc->getInputIndicesConnectedToThisNode(internalDst);
                for (std::list<int>::const_iterator it = inputsConnectedToOldSrc.begin(); it != inputsConnectedToOldSrc.end(); ++it) {
                    internalDst->disconnectInput(*it);
                }
            }
        } else if (internalOldSrc && !internalNewSrc) {
            std::list<int> inputsConnectedToOldSrc = internalOldSrc->getInputIndicesConnectedToThisNode(internalDst);
            for (std::list<int>::const_iterator it = inputsConnectedToOldSrc.begin(); it != inputsConnectedToOldSrc.end(); ++it) {
                internalDst->disconnectInput(*it);
            }
        } else if (!internalOldSrc && internalNewSrc) {
            Node::CanConnectInputReturnValue ret = internalDst->canConnectInput(internalNewSrc, inputNb);
            bool connectionOk = ret == Node::eCanConnectInput_ok ||
                                ret == Node::eCanConnectInput_differentFPS ||
                                ret == Node::eCanConnectInput_differentPars;
            if (connectionOk) {
                internalDst->connectInput(internalNewSrc, inputNb);
            } else {
                std::list<int> inputsConnectedToOldSrc = internalOldSrc->getInputIndicesConnectedToThisNode(internalDst);
                for (std::list<int>::const_iterator it = inputsConnectedToOldSrc.begin(); it != inputsConnectedToOldSrc.end(); ++it) {
                    internalDst->disconnectInput(*it);
                }
            }
        }
    }

    if (isViewer) {
        if (viewerInternalIndex == 0 || viewerInternalIndex == 1) {
            isViewer->connectInputToIndex(inputNb, viewerInternalIndex);
        }
    }

    dst->refreshEdges();
    dst->refreshEdgesVisility();

    if (newSrc) {
        newSrc->refreshEdgesVisility();
    }
    if (oldSrc) {
        oldSrc->refreshEdgesVisility();
    }
} // ConnectCommand::doConnect

InsertNodeCommand::InsertNodeCommand(NodeGraph* graph,
                                     Edge* edge,
                                     const NodeGuiPtr & newSrc,
                                     QUndoCommand *parent)
    : ConnectCommand(graph, edge, edge->getSource(), newSrc, -1, parent)
    , _inputEdge(0)
{
    assert(newSrc);
    setText( tr("Insert node") );
}

void
InsertNodeCommand::undo()
{
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    assert(newSrc);

    NodePtr oldSrcInternal = oldSrc ? oldSrc->getNode() : NodePtr();
    NodePtr newSrcInternal = newSrc->getNode();
    NodePtr dstInternal = dst->getNode();
    assert(newSrcInternal && dstInternal);

    doConnect(newSrc, oldSrc, dst, _inputNb, -1);

    if (_inputEdge) {
        doConnect( _inputEdge->getSource(), NodeGuiPtr(), _inputEdge->getDest(), _inputEdge->getInputNumber(), -1);
    }

    ViewerInstancePtr isDstAViewer = dst->getNode()->isEffectViewerInstance();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }
    _graph->update();
}

void
InsertNodeCommand::redo()
{
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    assert(newSrc);

    NodePtr oldSrcInternal = oldSrc ? oldSrc->getNode() : NodePtr();
    NodePtr newSrcInternal = newSrc->getNode();
    NodePtr dstInternal = dst->getNode();
    assert(newSrcInternal && dstInternal);

    newSrcInternal->beginInputEdition();
    dstInternal->beginInputEdition();

    doConnect(oldSrc, newSrc, dst, _inputNb, -1);


    ///find out if the node is already connected to what the edge is connected
    bool alreadyConnected = false;
    const std::vector<NodeWPtr > & inpNodes = newSrcInternal->getInputs();
    if (oldSrcInternal) {
        for (std::size_t i = 0; i < inpNodes.size(); ++i) {
            if (inpNodes[i].lock() == oldSrcInternal) {
                alreadyConnected = true;
                break;
            }
        }
    }

    _inputEdge = 0;
    if (oldSrcInternal && !alreadyConnected) {

        int prefInput = newSrcInternal->getPreferredInputForConnection();
        if (prefInput != -1) {
            _inputEdge = newSrc->getInputArrow(prefInput);
            assert(_inputEdge);
            doConnect( _inputEdge->getSource(), oldSrc, _inputEdge->getDest(), _inputEdge->getInputNumber(), -1 );
        }
    }

    ViewerInstancePtr isDstAViewer = dst->getNode()->isEffectViewerInstance();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }

    newSrcInternal->endInputEdition(false);
    dstInternal->endInputEdition(false);

   _graph->getGui()->getApp()->renderAllViewers();
    _graph->update();
} // InsertNodeCommand::redo

ResizeBackdropCommand::ResizeBackdropCommand(const NodeGuiPtr& bd,
                                             int w,
                                             int h,
                                             QUndoCommand *parent)
    : QUndoCommand(parent)
    , _bd(bd)
    , _w(w)
    , _h(h)
    , _oldW(0)
    , _oldH(0)
{
    QRectF bbox = bd->boundingRect();

    _oldW = bbox.width();
    _oldH = bbox.height();
}

ResizeBackdropCommand::~ResizeBackdropCommand()
{
}

void
ResizeBackdropCommand::undo()
{
    NodeGuiPtr bd = _bd.lock();
    if (!bd) {
        return;
    }
    bd->resize(_oldW, _oldH);
    setText( tr("Resize %1").arg( QString::fromUtf8( bd->getNode()->getLabel().c_str() ) ) );
}

void
ResizeBackdropCommand::redo()
{
    NodeGuiPtr bd = _bd.lock();
    if (!bd) {
        return;
    }
    bd->resize(_w, _h);
    setText( tr("Resize %1").arg( QString::fromUtf8( bd->getNode()->getLabel().c_str() ) ) );
}

bool
ResizeBackdropCommand::mergeWith(const QUndoCommand *command)
{
    const ResizeBackdropCommand* rCmd = dynamic_cast<const ResizeBackdropCommand*>(command);

    if (!rCmd) {
        return false;
    }
    if (rCmd->_bd.lock() != _bd.lock()) {
        return false;
    }
    _w = rCmd->_w;
    _h = rCmd->_h;

    return true;
}

DecloneMultipleNodesCommand::DecloneMultipleNodesCommand(NodeGraph* graph,
                                                         const  std::map<NodeGuiPtr, NodePtr> & nodes,
                                                         QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
{
    for ( std::map<NodeGuiPtr, NodePtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeToDeclone n;
        n.node = it->first;
        n.master = it->second;
        _nodes.push_back(n);
    }
}

DecloneMultipleNodesCommand::~DecloneMultipleNodesCommand()
{
}

void
DecloneMultipleNodesCommand::undo()
{
    for (std::list<NodeToDeclone>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->node.lock()->getNode()->linkToNode( it->master.lock());
    }

    _graph->getGui()->getApp()->triggerAutoSave();
    setText( tr("Declone node") );
}

void
DecloneMultipleNodesCommand::redo()
{
    for (std::list<NodeToDeclone>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->node.lock()->getNode()->unlinkAllKnobs();
    }

    _graph->getGui()->getApp()->triggerAutoSave();
    setText( tr("Declone node") );
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

typedef std::pair<NodeGuiPtr, QPointF> TreeNode; ///< all points are expressed as being the CENTER of the node!

class Tree
{
    std::list<TreeNode> nodes;
    QPointF topLevelNodeCenter; //< in scene coords

public:

    Tree()
        : nodes()
        , topLevelNodeCenter(0, INT_MAX)
    {
    }

    void buildTree(const NodeGuiPtr & output,
                   const NodesGuiList& selectedNodes,
                   std::list<NodeGui*> & usedNodes)
    {
        QPointF outputPos = output->pos();
        QSize nodeSize = output->getSize();

        outputPos += QPointF(nodeSize.width() / 2., nodeSize.height() / 2.);
        addNode(output, outputPos);

        buildTreeInternal(selectedNodes, output.get(), output->mapToScene( output->mapFromParent(outputPos) ), usedNodes);
    }

    const std::list<TreeNode> & getNodes() const
    {
        return nodes;
    }

    const QPointF & getTopLevelNodeCenter() const
    {
        return topLevelNodeCenter;
    }

    void moveAllTree(const QPointF & delta)
    {
        for (std::list<TreeNode>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            it->second += delta;
        }
    }

private:

    void addNode(const NodeGuiPtr & node,
                 const QPointF & point)
    {
        nodes.push_back( std::make_pair(node, point) );
    }

    void buildTreeInternal(const NodesGuiList& selectedNodes,
                           NodeGui* currentNode, const QPointF & currentNodeScenePos, std::list<NodeGui*> & usedNodes);
};

typedef std::list< boost::shared_ptr<Tree> > TreeList;

void
Tree::buildTreeInternal(const NodesGuiList& selectedNodes,
                        NodeGui* currentNode,
                        const QPointF & currentNodeScenePos,
                        std::list<NodeGui*> & usedNodes)
{
    QSize nodeSize = currentNode->getSize();
    NodePtr internalNode = currentNode->getNode();
    const std::vector<Edge*> & inputs = currentNode->getInputsArrows();
    NodeGuiPtr firstNonMaskInput;
    NodesGuiList otherNonMaskInputs;
    NodesGuiList maskInputs;

    for (U32 i = 0; i < inputs.size(); ++i) {
        NodeGuiPtr source = inputs[i]->getSource();

        ///Check if the source is selected
        NodesGuiList::const_iterator foundSelected = std::find(selectedNodes.begin(), selectedNodes.end(), source);
        if ( foundSelected == selectedNodes.end() ) {
            continue;
        }

        if (source) {
            bool isMask = internalNode->getEffectInstance()->isInputMask(i);
            if (!firstNonMaskInput && !isMask) {
                firstNonMaskInput = source;
                for (std::list<TreeNode>::iterator it2 = nodes.begin(); it2 != nodes.end(); ++it2) {
                    if (it2->first == firstNonMaskInput) {
                        firstNonMaskInput.reset();
                        break;
                    }
                }
            } else if (!isMask) {
                bool found = false;
                for (std::list<TreeNode>::iterator it2 = nodes.begin(); it2 != nodes.end(); ++it2) {
                    if (it2->first == source) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    otherNonMaskInputs.push_back(source);
                }
            } else if (isMask) {
                bool found = false;
                for (std::list<TreeNode>::iterator it2 = nodes.begin(); it2 != nodes.end(); ++it2) {
                    if (it2->first == source) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    maskInputs.push_back(source);
                }
            } else {
                ///this can't be happening
                assert(false);
            }
        }
    }


    ///The node has already been processed in another tree, skip it.
    if ( std::find(usedNodes.begin(), usedNodes.end(), currentNode) == usedNodes.end() ) {
        ///mark it
        usedNodes.push_back(currentNode);


        QPointF firstNonMaskInputPos;
        std::list<QPointF> otherNonMaskInputsPos;
        std::list<QPointF> maskInputsPos;

        ///Position the first non mask input
        if (firstNonMaskInput) {
            firstNonMaskInputPos = firstNonMaskInput->mapToParent( firstNonMaskInput->mapFromScene(currentNodeScenePos) );
            firstNonMaskInputPos.ry() -= ( (nodeSize.height() / 2.) +
                                           MINIMUM_VERTICAL_SPACE_BETWEEN_NODES +
                                           (firstNonMaskInput->getSize().height() / 2.) );

            ///and add it to the tree, with parent relative coordinates
            addNode(firstNonMaskInput, firstNonMaskInputPos);

            firstNonMaskInputPos = firstNonMaskInput->mapToScene( firstNonMaskInput->mapFromParent(firstNonMaskInputPos) );
        }

        ///Position all other non mask inputs
        int index = 0;
        for (NodesGuiList::iterator it = otherNonMaskInputs.begin(); it != otherNonMaskInputs.end(); ++it, ++index) {
            QPointF p = (*it)->mapToParent( (*it)->mapFromScene(currentNodeScenePos) );

            p.rx() -= ( (nodeSize.width() + (*it)->getSize().width() / 2.) ) * (index + 1);

            ///and add it to the tree
            addNode(*it, p);


            p = (*it)->mapToScene( (*it)->mapFromParent(p) );
            otherNonMaskInputsPos.push_back(p);
        }

        ///Position all mask inputs
        index = 0;
        for (NodesGuiList::iterator it = maskInputs.begin(); it != maskInputs.end(); ++it, ++index) {
            QPointF p = (*it)->mapToParent( (*it)->mapFromScene(currentNodeScenePos) );
            ///Note that here we subsctract nodeSize.width(): Actually we substract twice nodeSize.width() / 2: once to get to the left of the node
            ///and another time to add the space of half a node
            p.rx() += ( (nodeSize.width() + (*it)->getSize().width() / 2.) ) * (index + 1);

            ///and add it to the tree
            addNode(*it, p);

            p = (*it)->mapToScene( (*it)->mapFromParent(p) );
            maskInputsPos.push_back(p);
        }

        ///Now that we built the tree at this level, call this function again on the inputs that we just processed
        if (firstNonMaskInput) {
            buildTreeInternal(selectedNodes, firstNonMaskInput.get(), firstNonMaskInputPos, usedNodes);
        }

        std::list<QPointF>::iterator pointsIt = otherNonMaskInputsPos.begin();
        for (NodesGuiList::iterator it = otherNonMaskInputs.begin(); it != otherNonMaskInputs.end(); ++it, ++pointsIt) {
            buildTreeInternal(selectedNodes, it->get(), *pointsIt, usedNodes);
        }

        pointsIt = maskInputsPos.begin();
        for (NodesGuiList::iterator it = maskInputs.begin(); it != maskInputs.end(); ++it, ++pointsIt) {
            buildTreeInternal(selectedNodes, it->get(), *pointsIt, usedNodes);
        }
    }
    ///update the top level node center if the node doesn't have any input
    if ( !firstNonMaskInput && otherNonMaskInputs.empty() && maskInputs.empty() ) {
        ///QGraphicsView Y axis is top-->down oriented
        if ( currentNodeScenePos.y() < topLevelNodeCenter.y() ) {
            topLevelNodeCenter = currentNodeScenePos;
        }
    }
} // buildTreeInternal

static bool
hasNodeOutputsInList(const std::list<NodeGuiPtr >& nodes,
                     const NodeGuiPtr& node)
{
    OutputNodesMap outputs;
    node->getNode()->getOutputs(outputs);
    for (std::list<NodeGuiPtr >::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it != node) {
            NodePtr n = (*it)->getNode();
            OutputNodesMap::const_iterator foundOutput = outputs.find(n);
            if (foundOutput != outputs.end()) {
                return true;
            }
        }
    }

    return false;
}

static bool
hasNodeInputsInList(const std::list<NodeGuiPtr >& nodes,
                    const NodeGuiPtr& node)
{
    const std::vector<NodeWPtr >& inputs = node->getNode()->getInputs();
    bool foundInput = false;

    for (std::list<NodeGuiPtr >::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it != node) {
            NodePtr n = (*it)->getNode();

            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (inputs[i].lock() == n) {
                    foundInput = true;
                    break;
                }
            }
            if (foundInput) {
                break;
            }
        }
    }

    return foundInput;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT


RearrangeNodesCommand::RearrangeNodesCommand(const std::list<NodeGuiPtr > & nodes,
                                             QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
{
    ///1) Separate the nodes in trees they belong to, once a node has been "used" by a tree, mark it
    ///and don't try to reposition it for another tree
    ///2) For all trees : recursively position each nodes so that each input of a node is positionned as following:
    /// a) The first non mask input is positionned above the node
    /// b) All others non mask inputs are positionned on the left of the node, each one separated by the space of half a node
    /// c) All masks are positionned on the right of the node, each one separated by the space of half a node
    ///3) Move all trees so that they are next to each other and their "top level" node
    ///(the input that is at the highest position in the Y coordinate) is at the same
    ///Y level (node centers have the same Y)

    std::list<NodeGui*> usedNodes;

    ///A list of Tree
    ///Each tree is a lit of nodes with a boolean indicating if it was already positionned( "used" ) by another tree, if set to
    ///true we don't do anything
    /// Each node that doesn't have any output is a potential tree.
    TreeList trees;

    for (std::list<NodeGuiPtr >::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( !hasNodeOutputsInList( nodes, (*it) ) ) {
            boost::shared_ptr<Tree> newTree(new Tree);
            newTree->buildTree(*it, nodes, usedNodes);
            trees.push_back(newTree);
        }
    }

    ///For all trees find out which one has the top most level node
    QPointF topLevelPos(0, INT_MAX);
    for (TreeList::iterator it = trees.begin(); it != trees.end(); ++it) {
        const QPointF & treeTop = (*it)->getTopLevelNodeCenter();
        if ( treeTop.y() < topLevelPos.y() ) {
            topLevelPos = treeTop;
        }
    }

    ///now offset all trees to be top aligned at the same level
    for (TreeList::iterator it = trees.begin(); it != trees.end(); ++it) {
        QPointF treeTop = (*it)->getTopLevelNodeCenter();
        if (treeTop.y() == INT_MAX) {
            treeTop.setY( topLevelPos.y() );
        }
        QPointF delta( 0, topLevelPos.y() - treeTop.y() );
        if ( (delta.x() != 0) || (delta.y() != 0) ) {
            (*it)->moveAllTree(delta);
        }

        ///and insert the final result into the _nodes list
        const std::list<TreeNode> & treeNodes = (*it)->getNodes();
        for (std::list<TreeNode>::const_iterator it2 = treeNodes.begin(); it2 != treeNodes.end(); ++it2) {
            NodeToRearrange n;
            n.node = it2->first;
            QSize size = it2->first->getSize();
            n.newPos = it2->second - QPointF(size.width() / 2., size.height() / 2.);
            n.oldPos = it2->first->pos();
            _nodes.push_back(n);
        }
    }
}

void
RearrangeNodesCommand::undo()
{
    for (std::list<NodeToRearrange>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodeGuiPtr node = it->node.lock();
        if (!node) {
            continue;
        }
        node->refreshPosition(it->oldPos.x(), it->oldPos.y(), true);
    }
    setText( tr("Rearrange nodes") );
}

void
RearrangeNodesCommand::redo()
{
    for (std::list<NodeToRearrange>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodeGuiPtr node = it->node.lock();
        if (!node) {
            continue;
        }
        node->refreshPosition(it->newPos.x(), it->newPos.y(), true);
    }
    setText( tr("Rearrange nodes") );
}

DisableNodesCommand::DisableNodesCommand(const std::list<NodeGuiPtr > & nodes,
                                         QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
{
    for (std::list<NodeGuiPtr >::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
}

void
DisableNodesCommand::undo()
{
    for (std::list<boost::weak_ptr<NodeGui> >::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->getEffectInstance()->setNodeDisabled(false);
    }
    setText( tr("Disable nodes") );
}

void
DisableNodesCommand::redo()
{
    for (std::list<boost::weak_ptr<NodeGui> >::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->getEffectInstance()->setNodeDisabled(true);
    }
    setText( tr("Disable nodes") );
}

EnableNodesCommand::EnableNodesCommand(const std::list<NodeGuiPtr > & nodes,
                                       QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
{
    for (std::list<NodeGuiPtr > ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
}

void
EnableNodesCommand::undo()
{
    for (std::list<boost::weak_ptr<NodeGui> >::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->getEffectInstance()->setNodeDisabled(true);
    }
    setText( tr("Enable nodes") );
}

void
EnableNodesCommand::redo()
{
    for (std::list<boost::weak_ptr<NodeGui> >::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->getEffectInstance()->setNodeDisabled(false);
    }
    setText( tr("Enable nodes") );
}

RenameNodeUndoRedoCommand::RenameNodeUndoRedoCommand(const NodeGuiPtr & node,
                                                     const QString& oldName,
                                                     const QString& newName)
    : QUndoCommand()
    , _node(node)
    , _oldName(oldName)
    , _newName(newName)
{
    assert(node);
    setText( tr("Rename node") );
}

RenameNodeUndoRedoCommand::~RenameNodeUndoRedoCommand()
{
}

void
RenameNodeUndoRedoCommand::undo()
{
    NodeGuiPtr node = _node.lock();

    node->setName(_oldName);
}

void
RenameNodeUndoRedoCommand::redo()
{
    NodeGuiPtr node = _node.lock();

    node->setName(_newName);
}

static void
addTreeInputs(const std::list<NodeGuiPtr >& nodes,
              const NodeGuiPtr& node,
              ExtractedTree& tree,
              std::list<NodeGuiPtr >& markedNodes)
{
    if ( std::find(markedNodes.begin(), markedNodes.end(), node) != markedNodes.end() ) {
        return;
    }

    if ( std::find(nodes.begin(), nodes.end(), node) == nodes.end() ) {
        return;
    }

    if ( !hasNodeInputsInList(nodes, node) ) {
        ExtractedInput input;
        input.node = node;
        input.inputs = node->getNode()->getInputs();
        tree.inputs.push_back(input);
        markedNodes.push_back(node);
    } else {
        tree.inbetweenNodes.push_back(node);
        markedNodes.push_back(node);
        const std::vector<Edge*>& inputs = node->getInputsArrows();
        for (std::vector<Edge*>::const_iterator it2 = inputs.begin(); it2 != inputs.end(); ++it2) {
            NodeGuiPtr input = (*it2)->getSource();
            if (input) {
                addTreeInputs(nodes, input, tree, markedNodes);
            }
        }
    }
}

static void
extractTreesFromNodes(const std::list<NodeGuiPtr >& nodes,
                      std::list<ExtractedTree>& trees)
{
    std::list<NodeGuiPtr > markedNodes;

    for (std::list<NodeGuiPtr >::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        bool isOutput = !hasNodeOutputsInList(nodes, *it);
        if (isOutput) {
            ExtractedTree tree;
            tree.output.node = *it;
            NodePtr n = (*it)->getNode();
            OutputNodesMap outputs;
            n->getOutputs(outputs);
            for (OutputNodesMap::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                for (std::list<int>::const_iterator it3 = it2->second.begin(); it3 != it2->second.end(); ++it3) {
                    tree.output.outputs.push_back( std::make_pair(*it3, it2->first) );
                }
            }

            const std::vector<Edge*>& inputs = (*it)->getInputsArrows();
            for (U32 i = 0; i < inputs.size(); ++i) {
                NodeGuiPtr input = inputs[i]->getSource();
                if (input) {
                    addTreeInputs(nodes, input, tree, markedNodes);
                }
            }

            if ( tree.inputs.empty() ) {
                ExtractedInput input;
                input.node = *it;
                input.inputs = n->getInputs();
                tree.inputs.push_back(input);
            }

            trees.push_back(tree);
        }
    }
}

///////////////

ExtractNodeUndoRedoCommand::ExtractNodeUndoRedoCommand(NodeGraph* graph,
                                                       const std::list<NodeGuiPtr >& nodes)
    : QUndoCommand()
    , _graph(graph)
    , _trees()
{
    extractTreesFromNodes(nodes, _trees);
}

ExtractNodeUndoRedoCommand::~ExtractNodeUndoRedoCommand()
{
}

void
ExtractNodeUndoRedoCommand::undo()
{

    for (std::list<ExtractedTree>::iterator it = _trees.begin(); it != _trees.end(); ++it) {
        NodeGuiPtr output = it->output.node.lock();
        ///Connect and move output
        for (std::list<std::pair<int, NodeWPtr > >::iterator it2 = it->output.outputs.begin(); it2 != it->output.outputs.end(); ++it2) {
            NodePtr node = it2->second.lock();
            if (!node) {
                continue;
            }
            node->disconnectInput(it2->first);
            node->connectInput(output->getNode(), it2->first);
        }

        QPointF curPos = output->pos();
        output->refreshPosition(curPos.x() - 200, curPos.y(), true);

        ///Connect and move inputs
        for (std::list<ExtractedInput>::iterator it2 = it->inputs.begin(); it2 != it->inputs.end(); ++it2) {
            NodeGuiPtr input = it2->node.lock();
            for (U32 i  =  0; i < it2->inputs.size(); ++i) {
                if ( it2->inputs[i].lock() ) {
                    input->getNode()->connectInput(it2->inputs[i].lock(), i);
                }
            }

            if (input != output) {
                QPointF curPos = input->pos();
                input->refreshPosition(curPos.x() - 200, curPos.y(), true);
            }
        }

        ///Move all other nodes

        for (std::list<boost::weak_ptr<NodeGui> >::iterator it2 = it->inbetweenNodes.begin(); it2 != it->inbetweenNodes.end(); ++it2) {
            NodeGuiPtr node = it2->lock();
            QPointF curPos = node->pos();
            node->refreshPosition(curPos.x() - 200, curPos.y(), true);
        }

    }

    _graph->getGui()->getApp()->renderAllViewers();
    _graph->getGui()->getApp()->triggerAutoSave();
    setText( tr("Extract node") );
} // ExtractNodeUndoRedoCommand::undo

void
ExtractNodeUndoRedoCommand::redo()
{

    for (std::list<ExtractedTree>::iterator it = _trees.begin(); it != _trees.end(); ++it) {
        NodeGuiPtr output = it->output.node.lock();


        bool outputsAlreadyDisconnected = false;

        ///Reconnect outputs to the input of the input of the ExtractedInputs if inputs.size() == 1
        if ( (it->output.outputs.size() == 1) && (it->inputs.size() == 1) ) {
            const ExtractedInput& selectedInput = it->inputs.front();
            const std::vector<NodeWPtr > &inputs = selectedInput.inputs;
            NodeGuiPtr selectedInputNode = selectedInput.node.lock();
            NodePtr inputToConnectTo;
            for (U32 i = 0; i < inputs.size(); ++i) {
                if ( inputs[i].lock() && !selectedInputNode->getNode()->getEffectInstance()->isInputOptional(i) ) {
                    inputToConnectTo = inputs[i].lock();
                    break;
                }
            }

            if (inputToConnectTo) {
                for (std::list<std::pair<int, NodeWPtr > >::iterator it2 = it->output.outputs.begin(); it2 != it->output.outputs.end(); ++it2) {
                    NodePtr node = it2->second.lock();
                    if (!node) {
                        continue;
                    }
                    node->disconnectInput(it2->first);
                    node->connectInput(inputToConnectTo, it2->first);
                }
                outputsAlreadyDisconnected = true;
            }
        }

        ///Disconnect and move output
        if (!outputsAlreadyDisconnected) {
            for (std::list<std::pair<int, NodeWPtr > >::iterator it2 = it->output.outputs.begin(); it2 != it->output.outputs.end(); ++it2) {
                NodePtr node = it2->second.lock();
                if (node) {
                    node->disconnectInput(it2->first);
                }
            }
        }

        QPointF curPos = output->pos();
        output->refreshPosition(curPos.x() + 200, curPos.y(), true);


        ///Disconnect and move inputs
        for (std::list<ExtractedInput>::iterator it2 = it->inputs.begin(); it2 != it->inputs.end(); ++it2) {
            NodeGuiPtr node = it2->node.lock();
            for (U32 i  =  0; i < it2->inputs.size(); ++i) {
                if ( it2->inputs[i].lock() ) {
                    node->getNode()->disconnectInput(i);
                }
            }
            if (node != output) {
                QPointF curPos = node->pos();
                node->refreshPosition(curPos.x() + 200, curPos.y(), true);
            }
        }

        ///Move all other nodes

        for (std::list<boost::weak_ptr<NodeGui> >::iterator it2 = it->inbetweenNodes.begin(); it2 != it->inbetweenNodes.end(); ++it2) {
            NodeGuiPtr node = it2->lock();
            QPointF curPos = node->pos();
            node->refreshPosition(curPos.x() + 200, curPos.y(), true);
        }
    }

    _graph->getGui()->getApp()->renderAllViewers();
    _graph->getGui()->getApp()->triggerAutoSave();


    setText( tr("Extract node") );
} // ExtractNodeUndoRedoCommand::redo

GroupFromSelectionCommand::GroupFromSelectionCommand(const NodesList & nodes)
    : QUndoCommand()
    , _oldGroup()
    , _newGroup()
{
    setText( tr("Group from selection") );

    assert( !nodes.empty() );
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeCollectionPtr group = (*it)->getGroup();

        // All nodes must belong to the same group
        assert(!_oldGroup.lock() || _oldGroup.lock() == group);
        _oldGroup = group;
        _originalNodes.push_back(*it);
    }

}

GroupFromSelectionCommand::~GroupFromSelectionCommand()
{
}

void
GroupFromSelectionCommand::undo()
{
    std::list<NodeGuiPtr> nodesToSelect;

    // Restore all links to the selection
    for (LinksMap::iterator it = _savedLinks.begin(); it != _savedLinks.end(); ++it) {
        NodePtr node = it->first.lock();
        if (!node) {
            continue;
        }
        
        const std::vector<NodeWPtr>& oldInputs = it->second;
        for (std::size_t i = 0; i < oldInputs.size(); ++i) {
            node->swapInput(oldInputs[i].lock(), i);
        }

    }

    NodeCollectionPtr oldGroup = _oldGroup.lock();
    NodeGraph* oldGraph = dynamic_cast<NodeGraph*>(oldGroup->getNodeGraph());

    // Restore original selection
    for (NodesWList::iterator it = _originalNodes.begin(); it != _originalNodes.end(); ++it) {
        NodePtr node = it->lock();
        if (node) {
            node->moveToGroup(oldGroup);
            NodeGuiPtr nodeGui = toNodeGui(node->getNodeGui());
            if (nodeGui) {
                nodesToSelect.push_back(nodeGui);
            }
        }
    }
    oldGraph->setSelection(nodesToSelect);

    // Destroy the created group
    {
        NodePtr groupNode = _newGroup.lock();
        if (groupNode) {
            groupNode->destroyNode(true, false);
        }
        _newGroup.reset();
    }

} // GroupFromSelectionCommand::undo

void
GroupFromSelectionCommand::redo()
{
    // The group position will be at the centroid of all selected nodes
    QPointF groupPosition;

    NodesList originalNodes;
    for (NodesWList::const_iterator it = _originalNodes.begin(); it != _originalNodes.end(); ++it) {
        NodePtr n = it->lock();
        if (!n) {
            continue;
        }
        originalNodes.push_back(n);

        double x,y;
        n->getPosition(&x, &y);
        groupPosition.rx() += x;
        groupPosition.ry() += y;
    }

    unsigned sz = originalNodes.size();
    if (sz) {
        groupPosition.rx() /= sz;
        groupPosition.ry() /= sz;
    }


    // Create the actual Group node
    NodeCollectionPtr oldContainer = _oldGroup.lock();
    if (!oldContainer) {
        return;
    }
    NodeGraph* oldContainerGraph = dynamic_cast<NodeGraph*>(oldContainer->getNodeGraph());
    assert(oldContainerGraph);
    if (!oldContainerGraph) {
        return;
    }

    CreateNodeArgsPtr groupArgs(CreateNodeArgs::create(PLUGINID_NATRON_GROUP, oldContainer ));
    groupArgs->setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    groupArgs->setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    groupArgs->setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    groupArgs->setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);


    NodePtr containerNode = oldContainerGraph->getGui()->getApp()->createNode(groupArgs);

    NodeGroupPtr containerGroup = containerNode->isEffectNodeGroup();
    assert(containerGroup);
    if (!containerGroup) {
        return;
    }

    NodeGraph* newContainerGraph = dynamic_cast<NodeGraph*>(containerGroup->getNodeGraph());
    assert(newContainerGraph);
    if (!newContainerGraph) {
        return;
    }

    _newGroup = containerNode;


    // Set the position of the group
    containerNode->setPosition( groupPosition.x(), groupPosition.y() );

    // Move all the selected nodes to the newly created Group
    for (NodesList::const_iterator it = originalNodes.begin(); it!=originalNodes.end(); ++it) {
        (*it)->moveToGroup(containerGroup);
    }

    // Just moving nodes into the group is not enough: we need to create the appropriate number
    // of Input nodes in the group to match the input of the selection so that the graph is not broken
    // and undoing this operation would restore the state.
    std::list<Project::NodesTree> trees;
    Project::extractTreesFromNodes(originalNodes, trees);
    int inputNb = 0;


    // The output node position is the average of all trees root
    QPointF outputNodePosition = QPointF(0,0);

    for (std::list<Project::NodesTree>::iterator it = trees.begin(); it != trees.end(); ++it) {

        // For each input node of each tree branch within the group, add a Input node in input of that branch
        // to actually create the input on the Group node
        for (std::list<Project::TreeInput>::iterator it2 = it->inputs.begin(); it2 != it->inputs.end(); ++it2) {

            // For each connected input of the original node, create a corresponding Input node with the appropriate name
            const std::vector<NodeWPtr >& originalNodeInputs = it2->node->getInputs();
            _savedLinks[it2->node] = originalNodeInputs;
            for (std::size_t i = 0; i < originalNodeInputs.size(); ++i) {
                NodePtr originalInput = originalNodeInputs[i].lock();

                //Create an input node corresponding to this input
                CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_NATRON_INPUT, containerGroup));
                args->setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
                args->setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
                args->setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);


                NodePtr input = containerNode->getApp()->createNode(args);
                assert(input);

                // Name the Input node with the label of the node and the input label
                std::string inputLabel = it2->node->getLabel() + '_' + it2->node->getInputLabel(i);
                input->setLabel(inputLabel);

                // Position the input node correctly
                double offsetX, offsetY;



                double outputX, outputY;
                it->output.node->getPosition(&outputX, &outputY);
                if (originalInput) {
                    double inputX, inputY;
                    originalInput->getPosition(&inputX, &inputY);
                    offsetX = inputX - outputX;
                    offsetY = inputY - outputY;
                } else {
                    offsetX = outputX;
                    offsetY = outputY - 100;
                }
                double thisInputX, thisInputY;
                it2->node->getPosition(&thisInputX, &thisInputY);

                thisInputX += offsetX;
                thisInputY += offsetY;

                input->setPosition(thisInputX, thisInputY);

                it2->node->swapInput(input, i);
                if (originalInput) {

                    containerGroup->getNode()->connectInput(originalInput, inputNb);
                }
                ++inputNb;
                
            } // for all node's inputs


            double thisNodeX, thisNodeY;
            it->output.node->getPosition(&thisNodeX, &thisNodeY);
            double thisNodeW, thisNodeH;
            it->output.node->getSize(&thisNodeW, &thisNodeH);
            thisNodeY += thisNodeH * 2;
            outputNodePosition.rx() += thisNodeX;
            outputNodePosition.ry() += thisNodeY;

        } // for all inputs in the tree

        // Connect all outputs of the original node to the new Group
        OutputNodesMap originalOutputs;
        it->output.node->getOutputs(originalOutputs);
        for (OutputNodesMap::const_iterator it2 = originalOutputs.begin(); it2 != originalOutputs.end(); ++it2) {
            _savedLinks[it2->first] = it2->first->getInputs();

        }
    } // for all trees

    //Create only a single output

    {
        CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_NATRON_OUTPUT, containerGroup));
        args->setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        args->setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args->setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        NodePtr output = containerNode->getApp()->createNode(args);

        assert(output);

        if (trees.size() > 0) {
            outputNodePosition.rx() /= trees.size();
            outputNodePosition.ry() /= trees.size();
            output->setPosition(outputNodePosition.x(), outputNodePosition.y());
        }

        // If only a single tree, connect the output node to the bottom of the tree
        if (trees.size() == 1) {

            output->swapInput(trees.front().output.node, 0);
        }
    }


    // Select the group node in the old graph
    std::list<NodeGuiPtr> nodesToSelect;
    NodeGuiPtr nodeGroupGui = toNodeGui(containerNode->getNodeGui());
    if (nodeGroupGui) {
        nodesToSelect.push_back(nodeGroupGui);
    }
    oldContainerGraph->setSelection(nodesToSelect);


    // Ensure all viewers are refreshed
    containerNode->getApp()->renderAllViewers();

    // Center the new nodegraph on all its nodes
    if (newContainerGraph) {
        newContainerGraph->centerOnAllNodes();
    }
} // GroupFromSelectionCommand::redo

InlineGroupCommand::InlineGroupCommand(const NodeCollectionPtr& newGroup, const NodesList & groupNodes)
: QUndoCommand()
, _newGroup(newGroup)
, _oldGroups()
{
    setText( tr("Inline Group(s)") );

    for (NodesList::const_iterator it = groupNodes.begin(); it != groupNodes.end(); ++it) {
        NodeGroupPtr group = (*it)->isEffectNodeGroup();
        assert(group);
        if (!group) {
            continue;
        }
        InlinedGroup inlinedGroup;
        inlinedGroup.oldGroupNode = group;
        inlinedGroup.groupInputs = (*it)->getInputs();


        std::vector<NodePtr> inputNodes;
        group->getInputs(&inputNodes);


        NodePtr outputNode = group->getOutputNode();
        for (std::size_t i = 0; i < inputNodes.size(); ++i) {

            OutputNodesMap inputOutputs;
            inputNodes[i]->getOutputs(inputOutputs);
            for (OutputNodesMap::const_iterator it2 = inputOutputs.begin(); it2 != inputOutputs.end(); ++it2) {
                const NodePtr& inputOutput = it2->first;

                for (std::list<int>::const_iterator it3 = it2->second.begin(); it3 != it2->second.end(); ++it3) {
                    InlinedGroup::InputOutput p;
                    p.output = inputOutput;
                    p.inputNodes = inputOutput->getInputs();
                    p.inputIndex = i;
                    p.outputInputIndex = *it3;
                    inlinedGroup.inputsMap.push_back(p);
                }

            }

        }
        if (outputNode) {
            inlinedGroup.outputNodeInput = outputNode->getInput(0);
        }

        OutputNodesMap groupOutputs;
        (*it)->getOutputs(groupOutputs);
        for (OutputNodesMap::const_iterator it2 = groupOutputs.begin(); it2 != groupOutputs.end(); ++it2) {
            const NodePtr& groupOutput = it2->first;

            for (std::list<int>::const_iterator it3 = it2->second.begin(); it3 != it2->second.end(); ++it3) {
                InlinedGroup::GroupNodeOutput outp;
                outp.output = groupOutput;
                outp.inputIndex = *it3;
                outp.outputNodeInputs = groupOutput->getInputs();
                groupOutput->getPosition(&outp.position[0], &outp.position[1]);
                inlinedGroup.groupOutputs.push_back(outp);
            }
        }
        
        
        NodesList nodes = group->getNodes();
        // Only move the nodes that are not GroupInput and GroupOutput
        // Compute the BBox of the inlined nodes so we can make space
        {
            bool bboxSet = false;
            for (NodesList::iterator it2 = nodes.begin(); it2 != nodes.end(); ++it2) {
                GroupInputPtr inp = (*it2)->isEffectGroupInput();
                GroupOutputPtr output = (*it2)->isEffectGroupOutput();
                if ( !inp && !output) {

                    InlinedGroup::MovedNode mnode;
                    double x,y;
                    (*it2)->getPosition(&x, &y);
                    mnode.position[0] = x;
                    mnode.position[1] = y;
                    if (!bboxSet) {
                        inlinedGroup.movedNodesBbox.x1 = inlinedGroup.movedNodesBbox.x2 = x;
                        inlinedGroup.movedNodesBbox.y1 = inlinedGroup.movedNodesBbox.y2 = y;
                        bboxSet = true;
                    } else {
                        inlinedGroup.movedNodesBbox.x1 = std::min(inlinedGroup.movedNodesBbox.x1, x);
                        inlinedGroup.movedNodesBbox.x2 = std::max(inlinedGroup.movedNodesBbox.x2, x);
                        inlinedGroup.movedNodesBbox.y1 = std::min(inlinedGroup.movedNodesBbox.y1, y);
                        inlinedGroup.movedNodesBbox.y2 = std::max(inlinedGroup.movedNodesBbox.y2, y);
                    }
                    mnode.node = *it2;
                    inlinedGroup.movedNodes.push_back(mnode);
                }

            }
        }

        (*it)->getPosition(&inlinedGroup.groupNodePos[0], &inlinedGroup.groupNodePos[1]);

        _oldGroups.push_back(inlinedGroup);
    }

}

InlineGroupCommand::~InlineGroupCommand()
{
}

void
InlineGroupCommand::undo()
{

    AppInstancePtr app;
    for (std::list<InlinedGroup>::const_iterator it = _oldGroups.begin(); it != _oldGroups.end(); ++it) {
        NodeGroupPtr group = it->oldGroupNode.lock();
        if (!group) {
            continue;
        }
        app = group->getApp();

        // Re-activate the group node
        group->getNode()->activate(Node::eActivateFlagRestoreOutputs);

        // Re-position back all moved nodes
        for (std::list<InlinedGroup::MovedNode>::const_iterator it2 = it->movedNodes.begin(); it2 != it->movedNodes.end(); ++it2) {
            NodePtr movedNode = it2->node.lock();
            if (!movedNode) {
                continue;
            }
            movedNode->moveToGroup(group);
            movedNode->setPosition(it2->position[0], it2->position[1]);
        }

        
        // Re-connect all input outputs to the GroupInput
        for (std::vector<InlinedGroup::InputOutput>::const_iterator it2 = it->inputsMap.begin(); it2 != it->inputsMap.end(); ++it2) {
            NodePtr inputOutput = it2->output.lock();
            for (std::size_t i = 0; i < it2->inputNodes.size(); ++i) {
                inputOutput->swapInput(it2->inputNodes[i].lock(), i);
            }
        }

        // Re-connect the output node to the group output input
        NodePtr outputNode = group->getOutputNode();
        if (outputNode) {
            outputNode->swapInput(it->outputNodeInput.lock(), 0);
        }

        // Re-connect all Group node outputs
        for (std::list<InlinedGroup::GroupNodeOutput>::const_iterator it2 = it->groupOutputs.begin(); it2 != it->groupOutputs.end(); ++it2) {
            NodePtr output = it2->output.lock();
            if (!output) {
                continue;
            }
            for (std::size_t i = 0; i < it2->outputNodeInputs.size(); ++i) {
                output->swapInput(it2->outputNodeInputs[i].lock(), i);
            }
        }

    } // for each group

    if (app) {
        app->triggerAutoSave();
        app->renderAllViewers();
    }
}

void
InlineGroupCommand::redo()
{

    NodeCollectionPtr newGroup = _newGroup.lock();
    AppInstancePtr app;
    for (std::list<InlinedGroup>::const_iterator it = _oldGroups.begin(); it != _oldGroups.end(); ++it) {
        NodeGroupPtr group = it->oldGroupNode.lock();
        if (!group) {
            continue;
        }

        app = group->getApp();

        std::vector<NodePtr> groupInputs;
        NodePtr groupOutput = group->getOutputNode();
        group->getInputs(&groupInputs);



        // Remember the links from the Group node we are expending to its inputs and outputs

        // This is the y coord. of the bottom-most input
        double inputY = INT_MIN;
        for (std::size_t i = 0; i < it->groupInputs.size(); ++i) {
            NodePtr input = it->groupInputs[i].lock();
            if (!input) {
                continue;
            }
            double x,y;
            input->getPosition(&x, &y);

            // Qt coord system is top down
            inputY = std::max(y, inputY);
        }

        // This is the y coord of the top most output
        double outputY = INT_MAX;
        {
            NodePtr groupOutputInput = it->outputNodeInput.lock();
            if (groupOutputInput) {
                double x;
                groupOutputInput->getPosition(&x, &outputY);
            }
        }

        const double ySpaceAvailable = outputY  - inputY;
        const double ySpaceNeeded = it->movedNodesBbox.y2 - it->movedNodesBbox.y1 + TO_DPIX(100);

        //  Move recursively the outputs of the group nodes so that it does not overlap the inlining of the group
        const QRectF rectToClear(it->movedNodesBbox.x1, it->movedNodesBbox.y1, it->movedNodesBbox.x2 - it->movedNodesBbox.x1, ySpaceNeeded - ySpaceAvailable);

        for (std::list<InlinedGroup::GroupNodeOutput>::const_iterator it2 = it->groupOutputs.begin(); it2 != it->groupOutputs.end(); ++it2) {
            NodePtr groupOutput = it2->output.lock();
            if (!groupOutput) {
                continue;
            }
            NodeGuiPtr groupOutputGui = toNodeGui(groupOutput->getNodeGui());
            if (groupOutputGui) {
                groupOutputGui->moveBelowPositionRecursively(rectToClear);
            }

        }

        const QPointF bboxCenter( (it->movedNodesBbox.x1 + it->movedNodesBbox.x2) / 2., (it->movedNodesBbox.y1 + it->movedNodesBbox.y2) / 2. );

        // Move the node to the new group
        // Also move all created nodes by this delta to fit in the space we've just made
        for (std::list<InlinedGroup::MovedNode>::const_iterator it2 = it->movedNodes.begin(); it2 != it->movedNodes.end(); ++it2) {
            QPointF curPos(it2->position[0], it2->position[1]);
            QPointF delta = curPos - bboxCenter;
            curPos = QPointF(it->groupNodePos[0], it->groupNodePos[1]) + delta;
            NodePtr movedNode = it2->node.lock();
            if (movedNode) {
                movedNode->moveToGroup(newGroup);
                movedNode->setPosition(curPos.x(), curPos.y());
            }
        }

        // Connect all GroupInput output nodes to the original Group node inputs
        for (std::size_t i = 0; i < it->inputsMap.size(); ++i) {
            NodePtr inputOutput = it->inputsMap[i].output.lock();
            if (!inputOutput) {
                continue;
            }
            assert(it->inputsMap[i].inputIndex >= 0 && it->inputsMap[i].inputIndex < (int)it->groupInputs.size());
            inputOutput->swapInput(it->groupInputs[it->inputsMap[i].inputIndex].lock(), it->inputsMap[i].outputInputIndex);
        }

        // Connect all original group node outputs to the  outputNodeInput
        for (std::list<InlinedGroup::GroupNodeOutput>::const_iterator it2 = it->groupOutputs.begin(); it2 != it->groupOutputs.end(); ++it2) {
            NodePtr output = it2->output.lock();
            if (!output) {
                continue;
            }
            output->swapInput(it->outputNodeInput.lock(), it2->inputIndex);
        }

        // Deactivate the group node
        group->getNode()->deactivate(Node::eDeactivateFlagConnectOutputsToMainInput);
        
    } // for each group


    if (app) {
        app->triggerAutoSave();
        app->renderAllViewers();
    }
} // redo

RestoreNodeToDefaultCommand::RestoreNodeToDefaultCommand(const NodesGuiList & nodes)
: QUndoCommand()
, _nodes()
{
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeDefaults d;
        d.node = *it;
        d.serialization.reset(new SERIALIZATION_NAMESPACE::NodeSerialization);
        (*it)->getNode()->toSerialization(d.serialization.get());
        _nodes.push_back(d);
    }
    setText(tr("Restore node(s) to default"));
}

RestoreNodeToDefaultCommand::~RestoreNodeToDefaultCommand()
{
    
}

void
RestoreNodeToDefaultCommand::undo()
{
    for (std::list<NodeDefaults>::const_iterator it = _nodes.begin(); it!=_nodes.end(); ++it) {
        NodeGuiPtr node = it->node.lock();
        if (!node) {
            continue;
        }
        NodePtr internalNode = node->getNode();
        if (!internalNode) {
            continue;
        }
        internalNode->loadKnobsFromSerialization(*it->serialization, false);
    }
}

void
RestoreNodeToDefaultCommand::redo()
{
    for (std::list<NodeDefaults>::const_iterator it = _nodes.begin(); it!=_nodes.end(); ++it) {
        NodeGuiPtr node = it->node.lock();
        if (!node) {
            continue;
        }
        NodePtr internalNode = node->getNode();
        if (!internalNode) {
            continue;
        }
        internalNode->restoreNodeToDefaultState(CreateNodeArgsPtr());
    }
}



NATRON_NAMESPACE_EXIT;
