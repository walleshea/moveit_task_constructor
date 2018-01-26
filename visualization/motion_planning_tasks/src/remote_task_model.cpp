/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Bielefeld University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Bielefeld University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Robert Haschke */

#include <stdio.h>

#include "remote_task_model.h"
#include <moveit/task_constructor/container.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit_task_constructor_msgs/GetSolution.h>
#include <ros/console.h>
#include <ros/service_client.h>

#include <QApplication>
#include <QPalette>
#include <qglobal.h>

using namespace moveit::task_constructor;

namespace moveit_rviz_plugin {

enum NodeFlag {
	WAS_VISITED        = 0x01, // indicate that model should emit change notifications
	NAME_CHANGED       = 0x02, // indicate that name was manually changed
};
typedef QFlags<NodeFlag> NodeFlags;

struct RemoteTaskModel::Node {
	Node *parent_;
	std::vector<std::unique_ptr<Node>> children_;
	QString name_;
	InterfaceFlags interface_flags_;
	NodeFlags node_flags_;
	std::unique_ptr<RemoteSolutionModel> solutions_;

	inline Node(Node *parent) : parent_(parent) {
		solutions_.reset(new RemoteSolutionModel());
	}

	bool setName(const QString& name) {
		if (name == name_) return false;
		name_ = name;
		return true;
	}
};

// return Node* corresponding to index
RemoteTaskModel::Node* RemoteTaskModel::node(const QModelIndex &index) const
{
	if (!index.isValid())
		return root_;

	if (index.model() != this) {
		ROS_ERROR_NAMED("TaskModel", "invalid model in QModelIndex");
		return nullptr;
	}

	// internal pointer refers to parent node
	Node *parent = static_cast<Node*>(index.internalPointer());
	Q_ASSERT(index.row() >= 0 && (size_t)index.row() < parent->children_.size());
	return parent->children_.at(index.row()).get();
}

// return Node* corresponding to stage_id
RemoteTaskModel::Node* RemoteTaskModel::node(uint32_t stage_id) const
{
	auto it = id_to_stage_.find(stage_id);
	return (it == id_to_stage_.end()) ? nullptr : it->second;
}

// return QModelIndex corresponding to Node*
QModelIndex RemoteTaskModel::index(const Node *n) const
{
	if (n == root_)
		return QModelIndex();

	Node *parent = n->parent_;

	// the internal pointer refers to the parent node of n
	for (int row = 0, end = parent->children_.size(); row != end; ++row)
		if (parent->children_.at(row).get() == n)
			return createIndex(row, 0, parent);
	Q_ASSERT(false);
}

RemoteTaskModel::RemoteTaskModel(const planning_scene::PlanningSceneConstPtr &scene, QObject *parent)
   : BaseTaskModel(parent), root_(new Node(nullptr)), scene_(scene)
{
	id_to_stage_[0] = root_; // root node has ID 0
}

RemoteTaskModel::~RemoteTaskModel()
{
	delete root_;
}

void RemoteTaskModel::setSolutionClient(ros::ServiceClient *client)
{
	get_solution_client_ = client;
}

int RemoteTaskModel::rowCount(const QModelIndex &parent) const
{
	if (parent.column() > 0)
		return 0;

	Node *n = node(parent);
	if (!n) return 0; // invalid model in parent

	return n->children_.size();
}

QModelIndex RemoteTaskModel::index(int row, int column, const QModelIndex &parent) const
{
	if (column < 0 || column >= columnCount())
		return QModelIndex();

	Node *p = node(parent);
	if (!p || row < 0 || (size_t)row >= p->children_.size())
		return QModelIndex();

	p->children_[row]->node_flags_ |= WAS_VISITED;
	// the internal pointer refers to the parent node
	return createIndex(row, column, p);
}

QModelIndex RemoteTaskModel::parent(const QModelIndex &child) const
{
	if (!child.isValid())
		return QModelIndex();

	// the internal pointer refers to the parent node
	Node *p = static_cast<Node*>(child.internalPointer());
	Q_ASSERT(p);
	if (child.model() != this || p == root_)
		return QModelIndex();

	return this->index(p);
}

Qt::ItemFlags RemoteTaskModel::flags(const QModelIndex &index) const
{
	Qt::ItemFlags flags = BaseTaskModel::flags(index);
	if (index.column() == 0)
		flags |= Qt::ItemIsEditable; // name is editable
	return flags;
}

QVariant RemoteTaskModel::data(const QModelIndex &index, int role) const
{
	Node *n = node(index);
	if (!n) return QVariant(); // invalid model in index

	switch (role) {
	case Qt::EditRole:
	case Qt::DisplayRole:
		switch (index.column()) {
		case 0: return n->name_;
		case 1: return n->solutions_->numSuccessful();
		case 2: return n->solutions_->numFailed();
		}
		break;
	case Qt::ForegroundRole:
		if (index.column() == 0 && !index.parent().isValid())
			return (flags_ & IS_DESTROYED) ? QColor(Qt::red) : QApplication::palette().text().color();
		break;
	}

	return BaseTaskModel::data(index, role);
}

bool RemoteTaskModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	Node *n = node(index);
	if (!n || index.column() != 0 || role != Qt::EditRole)
		return false;
	n->setName(value.toString());
	n->node_flags_ |= NAME_CHANGED;
	dataChanged(index, index);
	return true;
}

void RemoteTaskModel::processStageDescriptions(const moveit_task_constructor_msgs::TaskDescription::_stages_type &msg)
{
	// iterate over descriptions and create new / update existing nodes where needed
	for (const auto &s : msg) {
		// find parent node for stage s, this should always exist
		auto parent_it = id_to_stage_.find(s.parent_id);
		if (parent_it == id_to_stage_.end()) {
			ROS_ERROR_NAMED("TaskListModel", "No parent found for stage %d (%s)", s.id, s.name.c_str());
			continue;
		}
		Node *parent = parent_it->second;

		Node*& n = id_to_stage_[s.id];
		if (!n) { // create a new Node if neccessary
			// only emit notify signal if parent node was ever visited
			bool notify = parent->node_flags_ & WAS_VISITED;
			QModelIndex parentIdx = index(parent);
			int row = parent->children_.size();

			if (notify) beginInsertRows(parentIdx, row, row);
			parent->children_.push_back(std::make_unique<Node>(parent));
			if (notify) endInsertRows();

			// store Node* in id_to_stage_
			n = parent->children_.back().get();
		}
		Q_ASSERT(n->parent_ == parent);

		// set content of stage
		bool changed = false;
		if (!(n->node_flags_ & NAME_CHANGED)) // avoid overwriting a manually changed name
			changed |= n->setName(QString::fromStdString(s.name));

		InterfaceFlags old_flags = n->interface_flags_;
		n->interface_flags_ = InterfaceFlags();
		for (auto f : {READS_START, READS_END, WRITES_NEXT_START, WRITES_PREV_END}) {
			if (s.flags & f) n->interface_flags_ |= f;
			else n->interface_flags_ &= ~f;
		}
		changed |= (n->interface_flags_ != old_flags);

		// emit notify about model changes when node was already visited
		if (changed && (n->node_flags_ & WAS_VISITED)) {
			QModelIndex idx = index(n);
			dataChanged(idx, idx.sibling(idx.row(), 2));
		}
	}

	if (msg.empty()) {
		flags_ |= IS_DESTROYED;
		dataChanged(index(0, 0), index(0, 2));
	}
}

void RemoteTaskModel::processStageStatistics(const moveit_task_constructor_msgs::TaskStatistics::_stages_type &msg)
{
	// iterate over statistics and update node's solutions where needed
	for (const auto &s : msg) {
		// find node for stage s, this should always exist
		auto it = id_to_stage_.find(s.id);
		if (it == id_to_stage_.end()) {
			ROS_ERROR_NAMED("TaskListModel", "No stage %d", s.id);
			continue;
		}
		Node *n = it->second;
		n->solutions_->processSolutionIDs(s.solved, s.failed, s.num_failed);

		// emit notify about model changes when node was already visited
		if (n->node_flags_ & WAS_VISITED) {
			QModelIndex idx = index(n);
			dataChanged(idx.sibling(idx.row(), 1), idx.sibling(idx.row(), 2));
		}
	}
}

DisplaySolutionPtr RemoteTaskModel::processSolutionMessage(const moveit_task_constructor_msgs::Solution &msg)
{
	DisplaySolutionPtr s(new DisplaySolution);
	s->setFromMessage(scene_->diff(), msg);

	// store sub solution data in model
	for (const auto& sub : msg.sub_solution) {
		if (sub.id == 0) continue;
		if (RemoteSolutionModel *m = getSolutionModel(sub.stage_id))
			m->setSolutionData(sub.id, sub.cost, QString());
	}
	for (const auto& sub : msg.sub_trajectory) {
		if (sub.id == 0) continue;
		if (RemoteSolutionModel *m = getSolutionModel(sub.stage_id))
			m->setSolutionData(sub.id, sub.cost, QString::fromStdString(sub.name));
	}

	// caching is only enabled for top-level solutions (stage_id == 1)
	// otherwise we would store PlanningScenes over and over
	if (!msg.sub_solution.empty() &&
	    msg.sub_solution.front().stage_id == 1 &&
	    msg.sub_solution.front().id != 0) {
		// cache solution for future use
		id_to_solution_[msg.sub_solution.front().id] = s;

		// cache DisplaySolutions for all individual sub trajectories
		uint i=0;
		for (const auto &t : msg.sub_trajectory) {
			if (t.id == 0) continue;  // invalid id
			DisplaySolutionPtr &sub = id_to_solution_.insert(std::make_pair(t.id, DisplaySolutionPtr())).first->second;
			if (!sub) sub.reset(new DisplaySolution(*s, i));
			i++;
		}
	}

	return s;
}

RemoteSolutionModel* RemoteTaskModel::getSolutionModel(uint32_t stage_id) const
{
	Node *n = node(stage_id);
	return n ? n->solutions_.get() : nullptr;
}

QAbstractItemModel* RemoteTaskModel::getSolutionModel(const QModelIndex &index)
{
	Node *n = node(index);
	if (!n) return nullptr;
	return n->solutions_.get();
}

DisplaySolutionPtr RemoteTaskModel::getSolution(const QModelIndex &index)
{
	Q_ASSERT(index.isValid());

	uint32_t id = index.sibling(index.row(), 0).data(Qt::UserRole).toUInt();
	auto it = id_to_solution_.find(id);
	if (it == id_to_solution_.cend()) {
		// TODO: try to assemble (and cache) the solution from known leaves
		// to avoid some communication overhead

		DisplaySolutionPtr result;
		if (!(flags_ & IS_DESTROYED) && get_solution_client_) {
			// request solution via service
			moveit_task_constructor_msgs::GetSolution srv;
			srv.request.solution_id = id;
			try {
				if (get_solution_client_->call(srv)) {
					id_to_solution_[id] = result = processSolutionMessage(srv.response.solution);
					return result;
				} else { // on failure mark remote task as destroyed: don't retrieve more solutions
					flags_ |= IS_DESTROYED;
				}
			} catch (const std::exception& e) {
				ROS_ERROR("exception: %s", e.what());
			}
		}
		return result;
	}
	return it->second;
}

namespace detail {
// SFINAE magic selects matching template: http://en.cppreference.com/w/cpp/language/sfinae

// method used for sorted_ container, requiring an additional dereference to access id
template <class T>
typename T::iterator findById(T& c, decltype((*c.cbegin())->id) id)
{
	return std::find_if(c.begin(), c.end(), [id](const typename T::value_type& item) {
		return item->id == id;
	});
}

// method used for data_ container, allowing for binary search
template <class T>
typename T::iterator findById(T& c, decltype((*c.cbegin()).id) id)
{
	typedef decltype((*c.cbegin()).id) val_type;
	auto p = std::__equal_range(c.begin(), c.end(), id,
	                            [](typename T::iterator it, val_type val){ return it->id < val; },
	                            [](val_type val, typename T::iterator it){ return val < it->id; });
	if (p.first == p.second) return c.end();  // id not found
	return p.first;
}
}

RemoteSolutionModel::RemoteSolutionModel(QObject *parent)
   : QAbstractTableModel(parent)
{
}

int RemoteSolutionModel::rowCount(const QModelIndex &parent) const
{
	return sorted_.size();
}

int RemoteSolutionModel::columnCount(const QModelIndex &parent) const
{
	return 3;
}

QVariant RemoteSolutionModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation == Qt::Horizontal) {
		switch (role) {
		case Qt::DisplayRole:
			switch (section) {
			case 0: return tr("#");
			case 1: return tr("cost");
			case 2: return tr("name");
			}
		case Qt::TextAlignmentRole:
			return section == 2 ? Qt::AlignLeft : Qt::AlignRight;
		}
	}
	return QAbstractItemModel::headerData(section, orientation, role);
}

QVariant RemoteSolutionModel::data(const QModelIndex &index, int role) const
{
	Q_ASSERT(index.isValid());
	Q_ASSERT(!index.parent().isValid());

	const Data &item = *sorted_[index.row()];

	switch (role) {
	case Qt::UserRole:
	case Qt::ToolTipRole:
		return item.id;

	case Qt::DisplayRole:
		switch(index.column()) {
		case 0: return item.creation_rank;
		case 1:
			if (std::isinf(item.cost)) return tr(u8"∞");
			if (std::isnan(item.cost)) return QVariant();
			return item.cost;
		case 2: return item.name;
		}

	case Qt::ForegroundRole:
		if (std::isinf(item.cost))
			return QColor(Qt::red);
		break;

	case Qt::TextAlignmentRole:
		return index.column() == 2 ? Qt::AlignLeft : Qt::AlignRight;
	}
	return QVariant();
}

void RemoteSolutionModel::setSolutionData(uint32_t id, float cost, const QString &name)
{
	// retrieve iterator and row corresponding to id
	auto sit = detail::findById(sorted_, id);
	int row = (sit != sorted_.end()) ? sit - sorted_.begin() : -1;
	auto it = (sit != sorted_.end()) ? *sit : detail::findById(data_, id);
	if (it == data_.end()) {
		ROS_WARN("solution id not found: %d", id);
		return;
	}

	QModelIndex tl, br;
	Data &item = *it;
	if (item.cost != cost) {
		item.cost = cost;
		tl = br = index(row, 1);
	}
	if (item.name != name) {
		item.name = name;
		br = index(row, 2);
		if (!tl.isValid())
			tl = br;
	}
	if (tl.isValid())
		Q_EMIT dataChanged(tl, br);
}

void RemoteSolutionModel::sort(int column, Qt::SortOrder order)
{
	if (sort_column_ == column && sort_order_ == order)
		return; // nothing to do

	sort_column_ = column;
	sort_order_ = order;

	sortInternal();
}

void RemoteSolutionModel::sortInternal()
{
	Q_EMIT layoutAboutToBeChanged();
	QModelIndexList old_indexes = persistentIndexList();
	std::vector<DataList::iterator> old_sorted_; std::swap(sorted_, old_sorted_);

	// create new order in sorted_
	for (auto it = data_.begin(), end = data_.end(); it != end; ++it)
		if (isVisible(*it)) sorted_.push_back(it);

	if (sort_column_ >= 0) {
		std::sort(sorted_.begin(), sorted_.end(), [this](const DataList::iterator& left, const DataList::iterator& right) {
			int comp = 0;
			switch (sort_column_) {
			case 1:  // cost order
				if (left->cost_rank < right->cost_rank) comp = -1;
				else if (left->cost_rank > right->cost_rank) comp = 1;
				break;
			case 2:  // name
				comp = left->name.compare(right->name);
				break;
			}
			if (comp == 0)  // if still undecided, id decides
				comp = (left->id < right->id ? -1 : 1);
			return (sort_order_ == Qt::AscendingOrder) ? (comp < 0) : (comp >= 0);
		});
	}

	// map old indexes to new ones
	std::map<int, int> old_to_new_row;
	QModelIndexList new_indexes;
	for (int i = 0, end = old_indexes.count(); i != end; ++i) {
		int old_row = old_indexes[i].row();
		auto it_inserted = old_to_new_row.insert(std::make_pair(old_row, -1));
		if (it_inserted.second) { // newly inserted: find new row index
			auto it = detail::findById(sorted_, old_sorted_[old_row]->id);
			if (it != sorted_.cend())
				it_inserted.first->second = it - sorted_.begin();
		}
		new_indexes.append(index(it_inserted.first->second, old_indexes[i].column()));
	}

	changePersistentIndexList(old_indexes, new_indexes);
	Q_EMIT layoutChanged();
}

// process solution ids received in stage statistics
void RemoteSolutionModel::processSolutionIDs(const std::vector<uint32_t> &successful,
                                             const std::vector<uint32_t> &failed,
                                             size_t num_failed)
{
	bool was_empty = data_.empty();
	auto last = --data_.end();

	// append new items to the end of data_
	processSolutionIDs(successful, true);
	processSolutionIDs(failed, false);

	// assign creation rank to new items
	uint32_t rank = was_empty ? 0 : last->creation_rank;
	for (auto it = was_empty ? data_.begin() : ++last, end = data_.end(); it != end; ++it)
		it->creation_rank = ++rank;

	// the task may not report failure ids (in failed),
	// but it may report the overall number of failures
	num_failed_data_ = failed.size(); // needed to compute number of successes
	num_failed_ = std::max(num_failed, num_failed_data_);

	sortInternal();
}

void RemoteSolutionModel::processSolutionIDs(const std::vector<uint32_t> &ids, bool successful)
{
	// ids are ordered by cost, insert them into data_ list sorted by id
	double default_cost = successful ? std::numeric_limits<double>::quiet_NaN()
	                                 : std::numeric_limits<double>::infinity();
	uint32_t cost_rank = 0;
	for (const uint32_t id : ids) {
		Data item(id, default_cost, successful ? ++cost_rank : std::numeric_limits<uint32_t>::max());
		// find id in available data_
		auto p = std::equal_range(data_.begin(), data_.end(), item);
		if (p.first == p.second) {  // new item
			data_.insert(p.second, std::move(item));
		} else {  // existing item: update cost rank
			Q_ASSERT(p.first->id == id);
			p.first->cost_rank = item.cost_rank;
		}
	}
}

bool RemoteSolutionModel::isVisible(const RemoteSolutionModel::Data &item) const
{
	return std::isnan(item.cost) || item.cost <= max_cost_;
}

}
