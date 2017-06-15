#include <fstream>
#include <string>
#include <random>  
#include <chrono>

#include "boost/scoped_ptr.hpp"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "google/protobuf/text_format.h"
#include "stdint.h"

#include "caffe/proto/caffe.pb.h"
#include "caffe/util/db.hpp"
#include "caffe/util/format.hpp"
#include "caffe/util/rng.hpp"

using namespace std;
using caffe::Datum;
using boost::scoped_ptr;
using std::string;
using std::pair;
namespace db = caffe::db;

DEFINE_string(feature, "", 
	"The name of feature file");
DEFINE_string(data, "", 
	"The name of corresponding data file");
DEFINE_string(dbname, "",
	"The name of data base");

DEFINE_string(pooling, "max",
	"The type of pooling {max or avg}");
DEFINE_string(backend, "lmdb",
	"The backend {lmdb, leveldb} for storing the result");
DEFINE_int32(number, 24,
	"The orientation number of model");
DEFINE_bool(shuffle, true,
	"Randomly shuffle the order of images and their labels");
DEFINE_bool(accuracy, false,
	"Calculate the accuray after pooling");

class data_item{
public:
	data_item(){}
	bool operator<(const data_item& a)
	{
		return this->name_ < a.name_;
	}

 	data_item& operator=(const data_item& a)
	{
		name_ = a.name_;
		label_ = a.label_;
		id_ = a.id_;
		return *this;
	}
public:
	string name_;
	int label_;
	int id_;
};

void convert_dataset(const vector<float>& features, 
	const vector<data_item>& datas) 
{
	vector<int> idx(datas.size());
	for (int i = 0; i < datas.size(); ++i) idx[i] = i;
	if (FLAGS_shuffle) {
		// randomly shuffle data
		LOG(INFO) << "Shuffling data";
		caffe::shuffle(idx.begin(), idx.end());
		
		// output shuffled data
		string data_file_shuffle = FLAGS_data;
		size_t p = data_file_shuffle.rfind('.');
		if (string::npos == p) p = data_file_shuffle.length();
		data_file_shuffle.insert(p, "_feature_shuffle");
		std::ofstream outfile(data_file_shuffle);
		for (int i = 0; i < datas.size(); ++i)
		{
			int id = idx[i];
			outfile << datas[id].name_ << " "
				<< datas[id].label_ << std::endl;
		}
		outfile.close();
	}

	// Create new DB
	LOG(INFO) << "Writing data to DB";
	scoped_ptr<db::DB> db(db::GetDB(FLAGS_backend));
	db->Open(FLAGS_dbname, db::NEW);
	scoped_ptr<db::Transaction> txn(db->NewTransaction());

	// Data buffer
	Datum datum;
	int count = 0;
	int channel = features.size() / datas.size();
	for (int i = 0; i < datas.size(); ++i)
	{
		int id = idx[i];
		datum.set_label(datas[id].label_);
		datum.set_data(features.data() + channel * id, channel*sizeof(float));
		datum.set_channels(channel);
		datum.set_height(1);
		datum.set_width(1);

		string key_str = caffe::format_int(i, 8) + "_" + datas[i].name_;
		string out;
		CHECK(datum.SerializeToString(&out));
		txn->Put(key_str, out);

		if (++count % 1000 == 0) {
			// Commit db
			txn->Commit();
			txn.reset(db->NewTransaction());
			LOG(INFO) << "Processed " << count << " files.";
		}
	}
	// write the last batch
	if (count % 1000 != 0) {
		txn->Commit();
		LOG(INFO) << "Processed " << count << " files.";
	}
	db->Close();
}

void get_contents(vector<float>& features, vector<data_item>& datas)
{
	// open the feature file
	ifstream infile_feature(FLAGS_feature, ios::binary);
	LOG(INFO) << "Open file : " << FLAGS_feature;
	CHECK(infile_feature) << "Can not open the specific feature file.";
	int num = 0, dim = 0;
	infile_feature.read((char*)(&num), sizeof(int));
	infile_feature.read((char*)(&dim), sizeof(int));
	CHECK(!(num%FLAGS_number)) << "Feature number error!";
	features.resize(num*dim);
	infile_feature.read((char*)features.data(), sizeof(float)*features.size());
	infile_feature.close();

	// open the data file
	ifstream infile_label(FLAGS_data);
	LOG(INFO) << "Open file : " << FLAGS_data;
	CHECK(infile_label) << "Can not open the specific label file.";
	string line;
	int n = 0;
	datas.resize(num);
	while (getline(infile_label, line)){
		size_t pos = line.find_last_of(' ');

		CHECK_LT(n, num) << "The feature & data file inconsisent.";
		data_item& data = datas[n];
		data.name_ = line.substr(0, pos);
		data.label_ = atoi(line.substr(pos + 1).c_str());
		data.id_ = n;
		n++;
	}
	LOG(INFO) << "A total of " << datas.size() << " octree files.";
	infile_label.close();

	// sorting 	
	sort(datas.begin(), datas.end());
}

void feature_pooling(vector<float>& features_pool, vector<data_item>& datas_pool,
	const vector<float>& features,	const vector<data_item>& datas)
{
	int num = datas.size();
	int dim = features.size() / num;

	// pooling - label
	int N = num / FLAGS_number;
	datas_pool.resize(N);
	for (int i = 0; i < N; ++i)
	{
		datas_pool[i] = datas[i*FLAGS_number];
		datas_pool[i].id_ = i;
		for (int j = 1; j < FLAGS_number; ++j)
		{
			CHECK_EQ(datas_pool[i].label_, datas[i*FLAGS_number + j].label_)
				<< "Label inconsistent";
		}
	}

	// pooling - feature
	features_pool.resize(N*dim);
	if (FLAGS_pooling == "max")
	{
		for (int i = 0; i < N; ++i)
		{
			for (int d = 0; d < dim; ++d)
			{
				features_pool[i*dim + d] = -1.0e20;
			}

			for (int j = 0; j < FLAGS_number; ++j)
			{
				int id = datas[i*FLAGS_number + j].id_;
				for (int d = 0; d < dim; ++d)
				{
					if (features[id*dim + d] > features_pool[i*dim + d])
						features_pool[i*dim + d] = features[id*dim + d];
				}
			}
		}
	}
	else if (FLAGS_pooling == "avg")
	{
		for (int i = 0; i < N; ++i)
		{
			for (int d = 0; d < dim; ++d)
			{
				features_pool[i*dim + d] = 0;
			}

			for (int j = 0; j < FLAGS_number; ++j)
			{
				int id = datas[i*FLAGS_number + j].id_;
				for (int d = 0; d < dim; ++d)
				{
					features_pool[i*dim + d] += features[id*dim + d];
				}
			}

			for (int d = 0; d < dim; ++d)
			{
				features_pool[i*dim + d] /= (float)FLAGS_number;
			}
		}
	}
	else{
		LOG(ERROR) << "Only support {max, avg} pooling : " << FLAGS_pooling;
	}
}

void calc_accuarcy(const vector<float>& features_pool, const vector<data_item>& datas_pool)
{
	int n = datas_pool.size();
	int dim = features_pool.size() / n;
	float accuracy = 0;
	vector<float> acc_cls(dim, 0);
	vector<float> acc_num(dim, 1.0e-20);
	for (int i = 0; i < n; ++i)
	{
		int label = 0;
		float prob = features_pool[i*dim];
		for (int j = 1; j < dim; ++j)
		{
			if (features_pool[i*dim + j] > prob)
			{
				prob = features_pool[i*dim + j];
				label = j;
			}
		}
		CHECK_LE(label, dim);
		if (datas_pool[i].label_ == label)
		{
			accuracy += 1.0f;
			acc_cls[datas_pool[i].label_] += 1.0f;
		}
		acc_num[datas_pool[i].label_] += 1.0f;
	}

	LOG(INFO) << "After " << FLAGS_pooling << " pooling";
	LOG(INFO) << "Category: \tId \tNumber \tAccuarcy";
	float avg_cls_acc = 0.0;
	for (int i = 0; i < dim; ++i)
	{
		acc_cls[i] /= acc_num[i];
		avg_cls_acc += acc_cls[i];
		LOG(INFO) << "Category:\t" << i << "\t" <<(int)acc_num[i] << "\t" << acc_cls[i];
	}
	avg_cls_acc /= (float)dim;
	accuracy /= (float)n;
	LOG(INFO) << "Average category:\t" << dim << "\t" << avg_cls_acc;
	LOG(INFO) << "Total:\t" << "All\t" << n << "\t" << accuracy;
	
}

int main(int argc, char** argv) 
{
	::google::InitGoogleLogging(argv[0]);	
	FLAGS_alsologtostderr = 1; // Print output to stderr (while still logging)
	gflags::SetUsageMessage("This script converts pool the feature and\n"
		"save the result to corresponding database file.\n");
	gflags::ParseCommandLineFlags(&argc, &argv, true);

	CHECK_GT(FLAGS_feature.size(), 0) << "Need a file contains feature.";
	CHECK_GT(FLAGS_data.size(), 0) << "Need a file contains label.";

	vector<float> features, features_pool;
	vector<data_item> datas, datas_pool;
	get_contents(features, datas);
	feature_pooling(features_pool, datas_pool, features, datas);
	if (!FLAGS_dbname.empty())
	{
		convert_dataset(features_pool, datas_pool);
	}

	if (FLAGS_accuracy)
	{
		calc_accuarcy(features_pool, datas_pool);
	}

	return 0;
}