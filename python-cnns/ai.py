# %%

# AI for Doom

# Importing the libraries
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torch.autograd import Variable

# Importing the other Python files
import experience_replay
import image_preprocessing

# Import paparazzi environment
from pprz_env import PaparazziGym

# Part 1 - Building the AI

# Making the brain


class CNN(nn.Module):

    def __init__(self, number_actions):
        super(CNN, self).__init__()
        self.convolution1 = nn.Conv2d(
            in_channels=1, out_channels=32, kernel_size=5)
        self.convolution2 = nn.Conv2d(
            in_channels=32, out_channels=32, kernel_size=3)
        self.convolution3 = nn.Conv2d(
            in_channels=32, out_channels=64, kernel_size=2)
        self.fc1 = nn.Linear(in_features=self.count_neurons(
            (1, 64, 64)), out_features=40)
        self.fc2 = nn.Linear(in_features=40, out_features=number_actions)

    def count_neurons(self, image_dim):
        x = Variable(torch.rand(1, *image_dim))
        x = F.relu(F.max_pool2d(self.convolution1(x), 3, 2))
        x = F.relu(F.max_pool2d(self.convolution2(x), 3, 2))
        x = F.relu(F.max_pool2d(self.convolution3(x), 3, 2))
        return x.data.view(1, -1).size(1)

    def forward(self, x):
        x = F.relu(F.max_pool2d(self.convolution1(x), 3, 2))
        x = F.relu(F.max_pool2d(self.convolution2(x), 3, 2))
        x = F.relu(F.max_pool2d(self.convolution3(x), 3, 2))
        x = x.view(x.size(0), -1)
        x = F.relu(self.fc1(x))
        x = self.fc2(x)

        return x

    def to_script(self, image_dim):
        x = Variable(torch.rand(1, *image_dim))

        # Export the model
        torch.onnx.export(self,               # model being run
                          # model input (or a tuple for multiple inputs)
                          x,
                          # where to save the model (can be a file or file-like object)
                          "trained.onnx",
                          export_params=True,        # store the trained parameter weights inside the model file
                          opset_version=10,          # the ONNX version to export the model to
                          do_constant_folding=True,  # whether to execute constant folding for optimization
                          input_names=['input'],   # the model's input names
                          output_names=['output'],  # the model's output names
                          dynamic_axes={'input': {0: 'batch_size'},    # variable lenght axes
                                        'output': {0: 'batch_size'}})

# Making the body


class SoftmaxBody(nn.Module):

    def __init__(self, T):
        super(SoftmaxBody, self).__init__()
        self.T = T

    def forward(self, outputs):
        probs = F.softmax(outputs * self.T)
        actions = probs.multinomial(1)  # WHAT?
        return actions

# Making the AI


class AI:

    def __init__(self, brain, body):
        self.brain = brain
        self.body = body

    def __call__(self, inputs):
        input = Variable(torch.from_numpy(np.array(inputs, dtype=np.float32)))
        output = self.brain(input)
        actions = self.body(output)
        return actions.data.numpy()


# Part 2 - Training the AI with Deep Convolutional Q-Learning

number_actions = 9

# Building an AI
cnn = CNN(number_actions)
softmax_body = SoftmaxBody(T=1.0)
ai = AI(brain=cnn, body=softmax_body)
env = PaparazziGym()

# Setting up Experience Replay
n_steps = experience_replay.NStepProgress(env=env, ai=ai, n_step=10)
memory = experience_replay.ReplayMemory(n_steps=n_steps, capacity=10000)

# Implementing Eligibility Trace


def eligibility_trace(batch):
    gamma = 0.99
    inputs = []
    targets = []
    for series in batch:
        input = Variable(torch.from_numpy(
            np.array(torch.cat((series[0].state, series[-1].state), 0), dtype=np.float32)))
        output = cnn(input)
        cumul_reward = 0.0 if series[-1].done else output[1].data.max()
        for step in reversed(series[:-1]):
            cumul_reward = step.reward + gamma * cumul_reward
        state = np.array(series[0].state)
        target = output[0].data
        target[series[0].action] = cumul_reward
        inputs.append(state[0])
        targets.append(target)

    return torch.from_numpy(np.array(inputs, dtype=np.float32)), torch.stack(targets)

# Making the moving average on 100 steps


class MA:
    def __init__(self, size):
        self.list_of_rewards = []
        self.size = size

    def add(self, rewards):
        if isinstance(rewards, list):
            self.list_of_rewards += rewards
        else:
            self.list_of_rewards.append(rewards)
        while len(self.list_of_rewards) > self.size:
            del self.list_of_rewards[0]

    def average(self):
        if len(self.list_of_rewards) == 0:  # issue
            return 0
        return np.mean(self.list_of_rewards)


ma = MA(100)

# Training the AI
loss = nn.MSELoss()
optimizer = optim.Adam(cnn.parameters(), lr=0.001)
nb_epochs = 100
for epoch in range(1, nb_epochs + 1):
    memory.run_steps(200)
    for batch in memory.sample_batch(128):
        inputs, targets = eligibility_trace(batch)
        inputs, targets = Variable(inputs), Variable(targets)
        predictions = cnn(inputs)
        loss_error = loss(predictions, targets)
        optimizer.zero_grad()
        loss_error.backward()
        optimizer.step()
    rewards_steps = n_steps.rewards_steps()
    ma.add(rewards_steps)
    avg_reward = ma.average()
    print("Epoch: %s, Average Reward: %s" % (str(epoch), str(avg_reward)))

    cnn.to_script((1, 64, 64))

    if avg_reward >= 1500:
        print("Congratulations, your AI wins")
        break

env.quit()


# %%
