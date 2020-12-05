import os
import sys

sys.path.append('.')
import time

import matplotlib.pyplot as plt
import numpy as np
import torch
from config import cfg
from cv_bridge import CvBridge
from data import make_data_loader
from engine.inference import inference
from modeling import baseline
from PIL import Image
from ptl_msgs.msg import ImageBlock
from std_msgs.msg import Int16
from torchvision import transforms
from utils import reid_metric
import rospy
from cv2 import cv2 as cv2


def build_model():
    num_classes = 1
    last_stride = 1
    pretrain_path = "/home/tim/market_resnet50_model_120_rank1_945.pth"
    model_neck = 'bnneck'
    neck_feat = "after"
    model_name = "resnet50"
    pretrain_choice = "self"
    model = baseline.Baseline(num_classes, last_stride, pretrain_path,
                              model_neck, neck_feat, model_name,
                              pretrain_choice)
    model.load_param(pretrain_path)
    model.eval()
    model.cuda()
    return model


def blur_detection(img):

    img_gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    res = cv2.Laplacian(img_gray, cv2.CV_64F, ksize=1)
    score = res.var()
    if score < 100.0:
        rospy.loginfo(
            "Image Blur Detected, discard this image for database maintaince!")
        return True
    else:
        rospy.loginfo("blur score: %f", score)
        return False


class Object():
    def __init__(self, id, feat):
        self.id = id
        self.feats = feat

    def add_feat(self, feat):
        feat.feats = torch.cat((self.feats, feat), 0)

    def is_full(self):
        if self.feats.shape[0] > 10:
            return True
        else:
            return False


class ReIDDatabase():
    def __init__(self):
        self.object_list = []
        self.feat_id_list = []
        self.feat_all = torch.tensor([]).cuda()
        self.object_num = 0

    def init_database(self, feats):
        for f in feats:
            self.add_new_object(f.unsqueeze(0))

    def add_new_object(self, feat):
        new_object = Object(self.object_num, feat)
        self.object_list.append(new_object)
        self.feat_id_list.append(new_object.id)
        if self.feat_all.shape[0] == 0:
            self.feat_all = feat
        else:
            self.feat_all = torch.cat((self.feat_all, feat), 0)
        self.object_num = self.object_num + 1
        return self.object_num - 1

    def add_new_feat(self, feats, ids):
        for f, i in feats, ids:
            distmat = reid_metric.re_ranking(f,
                                             self.object_list[i].feats,
                                             k1=20,
                                             k2=6,
                                             lambda_value=0.5).squeeze(0)
            rank = np.argsort(distmat, axis=0).squeeze()
            if distmat[rank[0]] > 0.15 & (not self.object_list[i].is_full()):
                # update global database
                self.feat_all = torch.cat((self.feat_all, f), 0)
                self.feat_id_list.append(i)

                #update local database
                self.object_list[i].feats = torch.cat(
                    (self.object_list[i].feats, f), 0)


'''
Database management strategies:
    before query: 
        - do nothing when the image is blur
        - the image with id != -1
            - remove the one with the height/width ratio is not good 
            - remove the one that database has been already full
        - the image with id = -1
            - do nothing
    
    after query:
        - the image with id != -1
            - remove the one too similar to the database
        - the image with id = 1
            - new image:
                - do nothing
            - old image:
                - remove the one with the height/width ratio is not good 
                - remove the one that database has been already full
                - remove the one too similar to the database

# Publisher
# need to publish the object with id=-1 back with new assigned id
'''


class ReIDNode():
    def __init__(self):
        self.model = build_model()
        self.database = ReIDDatabase()
        rospy.init_node('ReID', anonymous=True)
        self.pub = rospy.Publisher('/ptl_reid/reid_to_tracker',
                                   ImageBlock,
                                   queue_size=1)
        rospy.Subscriber('/ptl_tracker/tracker_to_reid',
                         ImageBlock,
                         self.tracker_loginfo_callback,
                         None,
                         queue_size=1)
        rospy.loginfo("Load ReID net successfully!")
        rospy.spin()

    def tracker_loginfo_callback(self, data):
        bridge = CvBridge()
        cvimg = bridge.imgmsg_to_cv2(data.img, "bgr8")
        # print(type(cvimg))
        # print(cvimg.shape)
        # img_gray = cv2.cvtColor(cvimg, cv2.COLOR_BGR2GRAY)
        if blur_detection(cvimg):
            return
        else:
            ids_update, bboxs_update, imgs_update = [], [], []
            ids_query, bboxs_query, imgs_query = [], [], []
            for (i, b) in zip(data.ids, data.bboxs):
                img_block = cvimg[b.data[1]:b.data[1] + b.data[3],
                                  b.data[0]:b.data[0] + b.data[2]]
                img_block = cv2.resize(img_block, (128, 256),
                                       interpolation=cv2.INTER_CUBIC)
                print(img_block.shape)
                img_block = img_block.transpose(2, 0, 1)
                print(img_block.shape)
                if i.data != -1:
                    # remove the one with id that does not meet the requirement
                    if (1 <= 1.0 * b.data[3] / b.data[2] <= 3) & (
                            not self.database.object_list[i.data].is_full()):
                        ids_update.append(i.data)
                        bboxs_update.append(b)
                        imgs_update.append(img_block)
                else:
                    bboxs_query.append(b)
                    imgs_query.append(img_block)

            # process img for query
            if len(imgs_query) != 0:
                feats_query = self.cal_feat(imgs_query)
                ids_query = self.query(feats_query)
                ids_msgs = []
                for i in ids_query:
                    id_msg = Int16()
                    id_msg.data = i
                    ids_msgs.append(id_msg)

                pub_msgs = ImageBlock()
                pub_msgs.img = data.img
                pub_msgs.bboxs = bboxs_query
                pub_msgs.ids = ids_msgs
                self.pub.publish(pub_msgs)

            # process img for update
            if len(imgs_update) != 0:
                feats_update = self.cal_feat(imgs_update)
                self.update_database(feats_update, ids_update)

    def cal_feat(self, img_block):
        initial_flag = False
        for ib in img_block:
            if initial_flag == False:
                input_tensor = torch.from_numpy(ib).unsqueeze(0).float()
                initial_flag = True
            else:
                torch.cat(
                    (input_tensor, torch.from_numpy(ib).unsqueeze(0).float()),
                    0)
        with torch.no_grad():
            input_tensor_cuda = input_tensor.cuda()
            feats = self.model(input_tensor_cuda)
        return feats

    def update_database(self, feats, ids):
        if self.database.object_num == 0:
            rospy.loginfo("Database initialized failed!!!!!")
        else:
            for (f, i) in zip(feats, ids, ids):
                dis = reid_metric.re_ranking(
                    f,
                    self.database.object_list[i].feats,
                    k1=20,
                    k2=6,
                    lambda_value=0.5)
                rank = np.argsort(dis, axis=1).squeeze()
                if dis[rank[0]] < 0.2:
                    self.database.add_new_feat(f, self.database.feat_id_list[
                        rank[0]])  # check the similarity of the database

    def query(self, feats):
        ids = []
        if self.database.object_num == 0:
            self.database.init_database(feats)
            ids = [i for i in range(feats.shape[0])]
        else:
            rospy.loginfo(feats.shape)
            rospy.loginfo(self.database.feat_all.shape)
            distmat = reid_metric.re_ranking(feats,
                                             self.database.feat_all,
                                             k1=20,
                                             k2=6,
                                             lambda_value=0.5)
            rank = np.argsort(distmat, axis=1)
            rospy.loginfo(rank)
            rospy.loginfo(rank.shape)
            for (f, dis) in zip(feats, distmat):
                rospy.loginfo(rank[0])
                rospy.loginfo(dis[rank[0]])
                if dis[rank[0]] < 0.2:
                    self.database.add_new_feat(f, self.database.feat_id_list[
                        rank[0]])  # check the similarity of the database
                    ids.append(self.database.feat_id_list[rank[0]])
                else:
                    rospy.loginfo(
                        "New object detected! Add it to the database!")
                    ids.append(self.database.add_new_object(f.unsqueeze(0)))
        return ids


if __name__ == '__main__':
    ReIDNode()
